/* n64.c : bus memoire, interfaces du RCP et demarrage HLE. */
#include "n64.h"
#include "jit.h"
#include "n64rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define CPU_HZ 93750000u

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ------------------------------------------------------------ interruptions */

static void update_irq(N64 *m) { vr4300_set_ip(&m->cpu, 2, (m->mi_intr & m->mi_mask) != 0); }
static void raise(N64 *m, uint32_t b) { m->mi_intr |= b; update_irq(m); }
static void lower(N64 *m, uint32_t b) { m->mi_intr &= ~b; update_irq(m); }

/* ------------------------------------------------------------ PIF / manettes */

static void pif_controller(N64 *m, int ch, const uint8_t *cmd, int tx, uint8_t *res, int rx, uint8_t *rxbyte)
{
    (void)tx;
    if (ch < 4) {
        if (!m->pad_connected[ch]) { *rxbyte |= 0x80; return; }
        switch (cmd[0]) {
        case 0x00: case 0xFF:                        /* identification */
            if (rx >= 3) { res[0] = 0x05; res[1] = 0x00; res[2] = 0x02; }   /* manette, pas de Pak */
            break;
        case 0x01:                                   /* lecture des boutons */
            if (rx >= 4) {
                res[0] = (uint8_t)(m->pad_buttons[ch] >> 8);
                res[1] = (uint8_t)m->pad_buttons[ch];
                res[2] = (uint8_t)m->pad_x[ch];
                res[3] = (uint8_t)m->pad_y[ch];
            }
            break;
        default:
            *rxbyte |= 0x80;                         /* Controller Pak : etape 4 */
        }
        return;
    }
    if (ch == 4 && m->eeprom_type) {
        switch (cmd[0]) {
        case 0x00: case 0xFF:
            if (rx >= 3) { res[0] = 0x00; res[1] = m->eeprom_type == 2 ? 0xC0 : 0x80; res[2] = 0x00; }
            break;
        case 0x04: {                                 /* lecture d'un bloc de 8 octets */
            uint32_t off = (uint32_t)cmd[1] * 8;
            if (off + 8 <= (m->eeprom_type == 2 ? 2048u : 512u) && rx >= 8) memcpy(res, m->eeprom + off, 8);
            break;
        }
        case 0x05: {                                 /* ecriture */
            uint32_t off = (uint32_t)cmd[1] * 8;
            if (off + 8 <= (m->eeprom_type == 2 ? 2048u : 512u)) { memcpy(m->eeprom + off, cmd + 2, 8); m->eeprom_dirty = 1; }
            if (rx >= 1) res[0] = 0;
            break;
        }
        default: *rxbyte |= 0x80;
        }
        return;
    }
    *rxbyte |= 0x80;
}

static void pif_process(N64 *m)
{
    uint8_t *r = m->pif_ram;
    int i = 0, ch = 0;
    /* les commandes restent dans la RAM du PIF : libultra ne les reecrit pas a
     * chaque trame, il relit seulement. Elles sont donc rejouees a chaque lecture. */
    while (i < 63) {
        int tx = r[i], rx;
        if (tx == 0xFE) break;
        if (tx == 0xFF || tx == 0xFD) { i++; continue; }
        if (tx == 0) { ch++; i++; continue; }
        tx &= 0x3F;
        if (i + 1 >= 63) break;
        rx = r[i + 1] & 0x3F;
        if (i + 2 + tx + rx > 63) break;
        r[i + 1] &= 0x3F;                            /* efface les erreurs precedentes */
        pif_controller(m, ch, &r[i + 2], tx, &r[i + 2 + tx], rx, &r[i + 1]);
        i += 2 + tx + rx;
        ch++;
    }
    r[63] = 0;
}

/* ------------------------------------------------------------ DMA */

static uint8_t cart_byte(N64 *m, uint32_t pa)
{
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {
        uint32_t off = pa - 0x10000000u;
        if (off < m->rom_size) return m->rom[off];
        return (uint8_t)(((off >> 1) & 0xFFFF) >> ((off & 1) ? 0 : 8));   /* bus ouvert */
    }
    if (pa >= 0x08000000u && pa < 0x08008000u && m->sram_enabled) return m->sram[pa - 0x08000000u];
    return 0;
}

static void pi_dma_to_rdram(N64 *m, uint32_t len)
{
    uint32_t dram = m->pi[0] & 0xFFFFFE, cart = m->pi[1], i;
    len = (len & 0xFFFFFF) + 1;
    for (i = 0; i < len && dram + i < m->rdram_size; i++) m->rdram[dram + i] = cart_byte(m, cart + i);
    if (m->cpu.jit) jit_invalidate(&m->cpu, dram, len);       /* du code a peut-etre ete charge ici */
    m->pi[0] = (dram + len + 7) & ~7u;
    m->pi[1] = (cart + len + 1) & ~1u;
    m->pi_dmas++;
    raise(m, MI_PI);
}

static void sp_dma(N64 *m, int to_rsp, uint32_t v)
{
    uint32_t len = ((v & 0xFFF) | 7) + 1, count = ((v >> 12) & 0xFF) + 1, skip = (v >> 20) & 0xFF8;
    uint32_t mem = m->sp_reg[0] & 0x1FF8, dram = m->sp_reg[1] & 0xFFFFF8, c, i;
    if (!to_rsp && m->cpu.jit) jit_invalidate(&m->cpu, dram, (len + skip) * count);
    for (c = 0; c < count; c++) {
        for (i = 0; i < len; i++) {
            uint32_t sa = (mem & 0x1000) | ((mem + i) & 0xFFF), da = dram + i;
            if (da >= m->rdram_size) continue;
            if (to_rsp) m->sp_mem[sa] = m->rdram[da];
            else m->rdram[da] = m->sp_mem[sa];
        }
        mem = (mem & 0x1000) | ((mem + len) & 0xFFF);
        dram += len + skip;
    }
    m->sp_reg[0] = mem;
    m->sp_reg[1] = dram;
}

/* tache graphique sur l'autre fil */
static void gfx_job(void *arg)
{
    N64 *m = (N64 *)arg;
    uint64_t t0 = m->clock_us ? m->clock_us() : 0;
    gsp_run(&m->gsp, m->ucode, m->gfx_job_dlist);
    m->gfx_job_us = m->clock_us ? m->clock_us() - t0 : 0;
}

static void gfx_finished(N64 *m)
{
    m->sp_reg[4] |= 0x1 | 0x2 | 0x200;
    raise(m, MI_SP);
    raise(m, MI_DP);                                  /* "full sync" de l'RDP */
}

/* attend que l'autre fil ait fini (sans rien signaler au jeu) */
static void gfx_wait(N64 *m)
{
    if (!m->gfx_pending || m->gfx_waited) return;
    m->gfx_worker->wait(m->gfx_worker);
    m->gfx_waited = 1;
    m->t_gfx += m->gfx_job_us;
}

void n64_set_gfx_worker(N64 *m, N64Worker *w)
{
    gfx_wait(m);                                      /* la tache en cours est finie : changement sans risque */
    m->gfx_worker = w;
}

/* attend la fin puis la signale au jeu (sauf si la tache est "suspendue" : elle le
 * sera quand le jeu la relancera) */
void n64_gfx_sync(N64 *m)
{
    if (!m->gfx_pending) return;
    gfx_wait(m);
    if (m->gfx_yielded) return;
    m->gfx_pending = 0;
    gfx_finished(m);
}

/* tache RSP en haut niveau : graphismes = microcode emule (gsp.c) + RDP logiciel */
static void sp_run_task(N64 *m)
{
    uint32_t type = be32(m->sp_mem + 0xFC0);
    if (m->gfx_pending && m->gfx_yielded) {
        if (type == 1 && (be32(m->sp_mem + 0xFC4) & 1)) {   /* OS_TASK_YIELDED : reprise de la tache */
            m->gfx_yielded = 0;                       /* sa fin sera signalee quand le dessin sera fini */
            return;
        }
        if (type == 1) { m->gfx_yielded = 0; n64_gfx_sync(m); }   /* nouvelle tache graphique : l'ancienne est finie */
        /* tache son pendant la suspension : le dessin continue en fond */
    } else n64_gfx_sync(m);                           /* un seul RSP : la tache precedente doit etre finie */
    if (type == 1) {
        uint32_t data = be32(m->sp_mem + 0xFD8), dsize = be32(m->sp_mem + 0xFDC);
        m->gfx_tasks++;
        m->last_dlist = be32(m->sp_mem + 0xFF0);
        if (m->ucode < 0 || m->ucode_addr != (data & 0x7FFFFF)) {
            m->ucode = gsp_detect_ucode(m->rdram, m->rdram_size, data, dsize);
            m->ucode_addr = data & 0x7FFFFF;
        }
        /* compte les taches (et non les trames VI) : un jeu a 30 images/s n'envoie
         * une tache qu'une trame sur deux, compter les trames sauterait tout */
        if (m->frameskip > 0 && (m->gfx_tasks % (uint32_t)(m->frameskip + 1)) != 1 % (uint32_t)(m->frameskip + 1)) {
            m->fs_skipped = 1;                        /* saut d'image : la liste n'est pas dessinee */
        } else if (m->gfx_enabled && m->gfx_worker) {
            /* dessinee en parallele ; la fin (interruptions SP et DP) est signalee
             * quand l'autre fil a termine, comme sur la vraie console */
            m->gfx_job_dlist = m->last_dlist & 0x7FFFFF;
            m->gfx_pending = 1;
            m->gfx_yielded = 0;
            m->gfx_waited = 0;
            m->gfx_worker->start(m->gfx_worker, gfx_job, m);
            return;
        } else if (m->gfx_enabled) {
            uint64_t t0 = m->clock_us ? m->clock_us() : 0;
            gsp_run(&m->gsp, m->ucode, m->last_dlist & 0x7FFFFF);
            if (m->clock_us) m->t_gfx += m->clock_us() - t0;
        }
        gfx_finished(m);
        return;
    } else if (type == 2) {
        uint32_t data = be32(m->sp_mem + 0xFD8);
        m->audio_tasks++;
        if (m->audio_abi < 0 || m->audio_ucode_addr != (data & 0x7FFFFF)) {
            m->audio_abi = aspd_detect(m->rdram, m->rdram_size, data, m->game_id);
            m->audio_ucode_addr = data & 0x7FFFFF;
        }
        {
            uint64_t t0 = m->clock_us ? m->clock_us() : 0;
            aspd_run(&m->aspd, m->rdram, m->rdram_size, m->audio_abi, be32(m->sp_mem + 0xFF0) & 0x7FFFFF, be32(m->sp_mem + 0xFF4));
            if (m->clock_us) m->t_audio += m->clock_us() - t0;
        }
    } else m->other_tasks++;
    m->sp_reg[4] |= 0x1 | 0x2 | 0x200;                /* halt, broke, signal 2 (tache terminee) */
    raise(m, MI_SP);
}

/* liste de commandes RDP envoyee directement (registres DP START/END) */
static void dp_process(N64 *m)
{
    uint32_t cur = m->dp_reg[2], end = m->dp_reg[1];
    int xbus = (m->dp_reg[3] & 1) != 0, guard = 0;
    gfx_wait(m);                                      /* l'RDP est partage avec la tache en cours */
    m->dp_lists++;
    while (cur < end && guard++ < 100000) {
        uint32_t w[4], i;
        int len;
        for (i = 0; i < 4; i++) {
            uint32_t a = cur + i * 4;
            w[i] = xbus ? be32(m->sp_mem + (a & 0xFFC)) : (a + 3 < m->rdram_size ? be32(m->rdram + a) : 0);
        }
        len = rdp_command_len(w[0]);
        if (m->gfx_enabled) {
            if (len == 2) rdp_command(&m->rdp, w[0], w[1], w[2], w[3]);
            else rdp_command(&m->rdp, w[0], w[1], 0, 0);
        }
        cur += (uint32_t)len * 8;
        if (m->rdp.full_sync) { m->rdp.full_sync = 0; raise(m, MI_DP); }
    }
    m->dp_reg[2] = end;
}

static void sp_status_write(N64 *m, uint32_t v)
{
    uint32_t *s = &m->sp_reg[4];
    int i;
    if (m->gfx_pending && !m->gfx_yielded && (v & 0x400)) {
        /* demande de "yield" (le jeu veut le RSP pour le son) : si le dessin est fini on le
         * signale ; sinon la tache est declaree suspendue (SIG1) mais continue en fond */
        if (m->gfx_waited || m->gfx_worker->done) n64_gfx_sync(m);
        else { m->gfx_yielded = 1; *s |= 0x1 | 0x2 | 0x100; raise(m, MI_SP); }
    } else if (m->gfx_pending && !m->gfx_yielded && (v & 0x2)) n64_gfx_sync(m);   /* arret force */
    if (v & 0x2) *s |= 0x1;
    if (v & 0x4) *s &= ~0x2u;
    if (v & 0x8) lower(m, MI_SP);
    if (v & 0x10) raise(m, MI_SP);
    if (v & 0x20) *s &= ~0x20u;
    if (v & 0x40) *s |= 0x20;
    if (v & 0x80) *s &= ~0x40u;
    if (v & 0x100) *s |= 0x40;
    for (i = 0; i < 8; i++) {
        if (v & (0x200u << (i * 2))) *s &= ~(0x80u << i);
        if (v & (0x400u << (i * 2))) *s |= (0x80u << i);
    }
    if ((v & 0x1) && (*s & 0x1)) { *s &= ~0x1u; sp_run_task(m); }
}

/* sortie du tampon AI : PCM stereo 16 bits big-endian -> 48 kHz */
static void ai_output(N64 *m, uint32_t addr, uint32_t len)
{
    int16_t out[1024];
    int n = 0;
    uint32_t i, frames = len / 4, step;
    if (!m->audio_out || !m->ai_freq) return;
    step = (uint32_t)(((uint64_t)m->ai_freq << 16) / 48000u);
    for (i = 0; i < frames; i++) {
        uint32_t a = (addr + i * 4) & 0xFFFFFF;
        int16_t l = a + 3 < m->rdram_size ? (int16_t)(m->rdram[a] << 8 | m->rdram[a + 1]) : 0;
        int16_t r = a + 3 < m->rdram_size ? (int16_t)(m->rdram[a + 2] << 8 | m->rdram[a + 3]) : 0;
        while (m->ai_phase < 0x10000) {
            int32_t f = (int32_t)m->ai_phase;
            out[n++] = (int16_t)(m->ai_prev[0] + (((l - m->ai_prev[0]) * f) >> 16));
            out[n++] = (int16_t)(m->ai_prev[1] + (((r - m->ai_prev[1]) * f) >> 16));
            m->ai_phase += step;
            if (n >= 1024) { m->audio_out(m->audio_ctx, out, n / 2); m->ai_samples_out += (uint32_t)n / 2; n = 0; }
        }
        m->ai_phase -= 0x10000;
        m->ai_prev[0] = l; m->ai_prev[1] = r;
    }
    if (n) { m->audio_out(m->audio_ctx, out, n / 2); m->ai_samples_out += (uint32_t)n / 2; }
}

/* demarre la lecture du tampon en tete de file */
static void ai_play_head(N64 *m)
{
    uint32_t dac = (m->ai[4] & 0x3FFF) + 1, len = m->ai_q_len[0];
    uint32_t vi_clock = m->pal ? 49656530u : 48681812u;
    uint32_t freq = vi_clock / dac;
    if (freq < 4000 || freq > 96000) freq = 32000;
    m->ai_freq = freq;
    m->ai_end_cycle = m->cpu.cycles + (uint64_t)(len / 4) * CPU_HZ / freq;
    ai_output(m, m->ai_q_addr[0], len);
    raise(m, MI_AI);
}

static void ai_start(N64 *m)
{
    uint32_t len = m->ai[1] & 0x3FFF8;
    if (!len || m->ai_busy >= 2) return;
    m->ai_q_addr[m->ai_busy] = m->ai[0] & 0xFFFFF8;
    m->ai_q_len[m->ai_busy] = len;
    m->ai_busy++;
    if (m->ai_busy == 1) ai_play_head(m);
}

static void ai_update(N64 *m)
{
    if (m->ai_busy && m->cpu.cycles >= m->ai_end_cycle) {
        m->ai_busy--;
        if (m->ai_busy) {
            m->ai_q_addr[0] = m->ai_q_addr[1];
            m->ai_q_len[0] = m->ai_q_len[1];
            ai_play_head(m);
        }
    }
}

/* ------------------------------------------------------------ registres */

static uint32_t reg_read(N64 *m, uint32_t pa)
{
    uint32_t r = (pa & 0xFFFFF) >> 2;
    if (pa < 0x03F00000u) return 0;
    if (pa < 0x04000000u) return 0;                                   /* registres RDRAM */
    if (pa < 0x04040000u) return be32(m->sp_mem + (pa & 0x1FFC));
    if (pa < 0x04080000u) {
        r = (pa & 0x1F) >> 2;                                         /* registres SP a 0x04040000 */
        if (pa < 0x04040020u) {
            if (r == 7) { uint32_t v = m->sp_reg[7]; m->sp_reg[7] = 1; return v; }   /* semaphore */
            if (r == 5 || r == 6) return 0;
            return m->sp_reg[r];
        }
        return 0;
    }
    if (pa < 0x04100000u) return m->sp_pc;
    if (pa < 0x04200000u) return r < 8 ? (r == 3 ? (m->dp_reg[3] | 0x80) : m->dp_reg[r]) : 0;
    if (pa >= 0x04300000u && pa < 0x04400000u) {
        switch (r) { case 0: return m->mi_mode; case 1: return 0x02020102; case 2: return m->mi_intr; case 3: return m->mi_mask; }
        return 0;
    }
    if (pa >= 0x04400000u && pa < 0x04500000u) return r < 14 ? m->vi[r] : 0;
    if (pa >= 0x04500000u && pa < 0x04600000u) {
        if (r == 1) {
            uint64_t left;
            if (!m->ai_busy || !m->ai_freq) return 0;
            left = m->ai_end_cycle > m->cpu.cycles ? m->ai_end_cycle - m->cpu.cycles : 0;
            return (uint32_t)(left * m->ai_freq / CPU_HZ * 4) & 0x3FFF8;
        }
        if (r == 3) return (m->ai_busy ? 0x40000000u : 0) | (m->ai_busy >= 2 ? 0x80000001u : 0);
        return r < 6 ? m->ai[r] : 0;
    }
    if (pa >= 0x04600000u && pa < 0x04700000u) {
        if (r == 4) return (m->mi_intr & MI_PI) ? 0x8 : 0;
        return r < 13 ? m->pi[r] : 0;
    }
    if (pa >= 0x04700000u && pa < 0x04800000u) return r < 8 ? m->ri[r] : 0;
    if (pa >= 0x04800000u && pa < 0x04900000u) {
        if (r == 6) return (m->mi_intr & MI_SI) ? 0x1000 : 0;
        return r < 7 ? m->si[r] : 0;
    }
    return 0;
}

static void reg_write(N64 *m, uint32_t pa, uint32_t v)
{
    uint32_t r = (pa & 0xFFFFF) >> 2;
    if (pa < 0x04000000u) return;
    if (pa < 0x04040000u) { wbe32(m->sp_mem + (pa & 0x1FFC), v); return; }
    if (pa < 0x04080000u) {
        if (pa >= 0x04040020u) return;
        switch ((pa & 0x1F) >> 2) {
        case 0: m->sp_reg[0] = v & 0x1FF8; break;
        case 1: m->sp_reg[1] = v & 0xFFFFF8; break;
        case 2: sp_dma(m, 1, v); break;
        case 3: sp_dma(m, 0, v); break;
        case 4: sp_status_write(m, v); break;
        case 7: m->sp_reg[7] = 0; break;
        }
        return;
    }
    if (pa < 0x04100000u) { m->sp_pc = v & 0xFFC; return; }
    if (pa < 0x04200000u) {
        if (r == 0) { m->dp_reg[0] = m->dp_reg[2] = v & 0xFFFFF8; }
        else if (r == 3) { if (v & 1) m->dp_reg[3] &= ~1u; if (v & 2) m->dp_reg[3] |= 1; }
        else if (r == 1) { m->dp_reg[1] = v & 0xFFFFF8; dp_process(m); }
        return;
    }
    if (pa >= 0x04300000u && pa < 0x04400000u) {
        if (r == 0) {
            m->mi_mode = (m->mi_mode & ~0x7Fu) | (v & 0x7F);
            if (v & 0x80) m->mi_mode &= ~0x80u;
            if (v & 0x100) m->mi_mode |= 0x80;
            if (v & 0x200) m->mi_mode &= ~0x100u;
            if (v & 0x400) m->mi_mode |= 0x100;
            if (v & 0x800) lower(m, MI_DP);
            if (v & 0x1000) m->mi_mode &= ~0x200u;
            if (v & 0x2000) m->mi_mode |= 0x200;
        } else if (r == 3) {
            int i;
            for (i = 0; i < 6; i++) {
                if (v & (1u << (i * 2))) m->mi_mask &= ~(1u << i);
                if (v & (2u << (i * 2))) m->mi_mask |= (1u << i);
            }
            update_irq(m);
        }
        return;
    }
    if (pa >= 0x04400000u && pa < 0x04500000u) {
        if (r == 4) { lower(m, MI_VI); return; }
        if (r < 14) m->vi[r] = v;
        return;
    }
    if (pa >= 0x04500000u && pa < 0x04600000u) {
        if (r == 3) { lower(m, MI_AI); return; }
        if (r < 6) m->ai[r] = v;
        if (r == 1) ai_start(m);
        return;
    }
    if (pa >= 0x04600000u && pa < 0x04700000u) {
        if (r == 2) {                                                  /* RDRAM -> cartouche (SRAM) */
            uint32_t dram = m->pi[0] & 0xFFFFFE, cart = m->pi[1], len = (v & 0xFFFFFF) + 1, i;
            m->pi[2] = v;
            if (m->sram_enabled && cart >= 0x08000000u && cart < 0x08008000u) {
                for (i = 0; i < len && cart - 0x08000000u + i < 0x8000u && dram + i < m->rdram_size; i++)
                    m->sram[cart - 0x08000000u + i] = m->rdram[dram + i];
                m->sram_dirty = 1;
            }
            m->pi[0] = (dram + len + 7) & ~7u;
            m->pi[1] = (cart + len + 1) & ~1u;
            m->pi_dmas++;
            raise(m, MI_PI);
            return;
        }
        if (r == 3) { m->pi[3] = v; pi_dma_to_rdram(m, v); return; }
        if (r == 4) { if (v & 2) lower(m, MI_PI); return; }
        if (r < 13) m->pi[r] = v;
        return;
    }
    if (pa >= 0x04700000u && pa < 0x04800000u) { if (r < 8) m->ri[r] = v; return; }
    if (pa >= 0x04800000u && pa < 0x04900000u) {
        if (r == 0) m->si[0] = v & 0xFFFFFC;
        else if (r == 1) {                                             /* PIF -> RDRAM */
            uint32_t d = m->si[0], i;
            pif_process(m);
            for (i = 0; i < 64 && d + i < m->rdram_size; i++) m->rdram[d + i] = m->pif_ram[i];
            if (m->cpu.jit) jit_invalidate(&m->cpu, d, 64);
            m->si_dmas++;
            raise(m, MI_SI);
        } else if (r == 4) {                                           /* RDRAM -> PIF */
            uint32_t d = m->si[0], i;
            for (i = 0; i < 64 && d + i < m->rdram_size; i++) m->pif_ram[i] = m->rdram[d + i];
            m->si_dmas++;
            raise(m, MI_SI);
        } else if (r == 6) lower(m, MI_SI);
        return;
    }
}

/* ------------------------------------------------------------ bus du CPU (hors RDRAM) */

static uint32_t read32(N64 *m, uint32_t pa)
{
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {
        uint32_t off = pa - 0x10000000u;
        if (off + 4 <= m->rom_size) return be32(m->rom + off);
        return ((off & 0xFFFF) << 16) | (off & 0xFFFF);
    }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) return be32(m->pif_ram + (pa & 0x3C));
    if (pa >= 0x08000000u && pa < 0x08008000u && m->sram_enabled) return be32(m->sram + (pa & 0x7FFC));
    if (pa >= 0x08000000u && pa < 0x10000000u) return 0;
    if (pa >= 0x03F00000u && pa < 0x04900000u) return reg_read(m, pa & ~3u);
    return 0;
}

static uint64_t bus_read(void *ctx, uint32_t pa, int size)
{
    N64 *m = (N64 *)ctx;
    uint32_t w;
    if (size == 8) return (uint64_t)read32(m, pa) << 32 | read32(m, pa + 4);
    w = read32(m, pa & ~3u);
    if (size == 4) return w;
    if (size == 2) return (w >> ((2 - (pa & 2)) * 8)) & 0xFFFF;
    return (w >> ((3 - (pa & 3)) * 8)) & 0xFF;
}

static void bus_write(void *ctx, uint32_t pa, int size, uint64_t v)
{
    N64 *m = (N64 *)ctx;
    if (size == 8) { bus_write(ctx, pa, 4, v >> 32); bus_write(ctx, pa + 4, 4, (uint32_t)v); return; }
    if ((pa >= 0x04000000u && pa < 0x04002000u) || (pa >= 0x1FC007C0u && pa < 0x1FC00800u)) {
        uint8_t *p = pa < 0x04002000u ? m->sp_mem + (pa & 0x1FFF) : m->pif_ram + (pa & 0x3F);
        if (size == 1) p[0] = (uint8_t)v;
        else if (size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        else wbe32(p, (uint32_t)v);
        return;
    }
    if (size != 4) {                                   /* ecritures partielles sur registres : replace dans le mot */
        unsigned sh = size == 2 ? (2 - (pa & 2)) * 8 : (3 - (pa & 3)) * 8;
        v = (uint32_t)v << sh;
    }
    if (pa >= 0x03F00000u && pa < 0x04900000u) reg_write(m, pa & ~3u, (uint32_t)v);
}

/* ------------------------------------------------------------ API */

int n64_init(N64 *m, uint8_t *rom, uint32_t size, int cic, int pal, int ram8mb, int save_type)
{
    memset(m, 0, sizeof *m);
    m->rdram_size = ram8mb ? 8u << 20 : 4u << 20;
    m->rdram = (uint8_t *)calloc(m->rdram_size, 1);
    if (!m->rdram) return -1;
    m->rom = rom;
    m->rom_size = size;
    m->cic = cic;
    m->pal = pal;
    m->eeprom_type = save_type == SAVE_EEPROM4K ? 1 : save_type == SAVE_EEPROM16K ? 2 : 0;
    memset(m->eeprom, 0xFF, sizeof m->eeprom);
    m->sram_enabled = save_type == SAVE_SRAM;
    memset(m->sram, 0xFF, sizeof m->sram);
    m->audio_abi = -1;
    if (size >= 0x40) { m->game_id[0] = (char)rom[0x3C]; m->game_id[1] = (char)rom[0x3D]; }
    m->pad_connected[0] = 1;
    m->cycles_per_frame = pal ? CPU_HZ / 50 : CPU_HZ / 60;
    m->cpu.rdram = m->rdram;
    m->cpu.rdram_size = m->rdram_size;
    m->cpu.ctx = m;
    m->cpu.read = bus_read;
    m->cpu.write = bus_write;
    m->cpu.cpi = 2;                                   /* comme les emulateurs courants */
    m->cpu.idle_skip = 1;
    vr4300_reset(&m->cpu);
    if (rdp_init(&m->rdp, m->rdram, m->rdram_size)) { free(m->rdram); m->rdram = NULL; return -1; }
    gsp_init(&m->gsp, m->rdram, m->rdram_size, &m->rdp);
    m->gfx_enabled = 1;
    m->ucode = -1;
    return 0;
}

void n64_free(N64 *m)
{
    gfx_wait(m);
    jit_free(&m->cpu);
    rdp_free(&m->rdp);
    free(m->rdram);
    m->rdram = NULL;
}

void n64_boot_hle(N64 *m)
{
    VR4300 *c = &m->cpu;
    uint32_t entry, dst, n, i;
    uint8_t seed = m->cic == 6103 ? 0x78 : m->cic == 6105 ? 0x91 : m->cic == 6106 ? 0x85 : 0x3F;

    /* IPL1/IPL2 : les 4 premiers Ko de la cartouche vont dans la DMEM du RSP */
    memcpy(m->sp_mem, m->rom, m->rom_size < 0x1000 ? m->rom_size : 0x1000);
    /* IPL3 : copie 1 Mo depuis 0x1000 vers l'adresse de demarrage */
    entry = m->rom_size >= 12 ? be32(m->rom + 8) : 0x80000400u;
    if (m->cic == 6103) entry -= 0x100000;
    if (m->cic == 6106) entry -= 0x200000;
    dst = entry & 0x1FFFFFFF;
    n = m->rom_size > 0x1000 ? m->rom_size - 0x1000 : 0;
    if (n > 0x100000) n = 0x100000;
    for (i = 0; i < n && dst + i < m->rdram_size; i++) m->rdram[dst + i] = m->rom[0x1000 + i];

    /* variables du systeme lues par libultra */
    wbe32(m->rdram + 0x300, m->pal ? 0 : 1);          /* osTvType */
    wbe32(m->rdram + 0x304, 0);                       /* osRomType */
    wbe32(m->rdram + 0x308, 0xB0000000u);             /* osRomBase */
    wbe32(m->rdram + 0x30C, 0);                       /* osResetType */
    wbe32(m->rdram + 0x314, 0);                       /* osVersion */
    wbe32(m->rdram + 0x318, m->rdram_size);           /* osMemSize */
    if (m->cic == 6105) {
        static const uint32_t imem[8] = { 0x3C0DBFC0, 0x8DA807FC, 0x25AD07C0, 0x31080080,
                                          0x5500FFFC, 0x3C0DBFC0, 0x8DA80024, 0x3C0BB000 };
        wbe32(m->rdram + 0x3F0, m->rdram_size);
        for (i = 0; i < 8; i++) wbe32(m->sp_mem + 0x1000 + i * 4, imem[i]);
    }
    m->pif_ram[0x26] = seed;
    m->pif_ram[0x27] = 0x3F;

    /* registres (etat a la sortie du PIF, cf. n64.readthedocs.io) */
    c->r[11] = 0xFFFFFFFFA4000040ull;                 /* t3 */
    c->r[20] = m->pal ? 0 : 1;                        /* s4 : type de televiseur */
    c->r[22] = seed;                                  /* s6 : graine CIC */
    c->r[29] = 0xFFFFFFFFA4001FF0ull;                 /* sp */
    c->r[31] = 0xFFFFFFFFA4001550ull;                 /* ra */
    c->cp0[CP0_RANDOM] = 0x1F;
    c->cp0[CP0_STATUS] = 0x34000000;
    c->cp0[CP0_PRID] = 0x00000B00;
    c->cp0[CP0_CONFIG] = 0x0006E463;
    c->pc = entry;
    c->next_pc = entry + 4;

    m->sp_reg[4] = 0x1;                               /* RSP a l'arret */
    m->vi[6] = m->pal ? 625 : 525;                    /* V_SYNC */
    m->vi[3] = 0x3FF;
    m->ri[0] = 0xE; m->ri[1] = 0x40; m->ri[3] = 0x14; m->ri[4] = 0x63634;
    if (c->jit) jit_flush(c);
}

int n64_run(N64 *m, int max_halflines)
{
    int lines = (int)(m->vi[6] & 0x3FF);
    uint64_t per;
    if (lines < 100) lines = m->pal ? 625 : 525;
    per = m->cycles_per_frame / (uint64_t)((lines + 1) / 2);
    while (max_halflines-- > 0) {
        if (m->gfx_pending && !m->gfx_yielded && (m->gfx_waited || m->gfx_worker->done)) n64_gfx_sync(m);
        m->vi[4] = (uint32_t)m->line & ~1u;
        if (((m->vi[3] & 0x3FF) & ~1u) == (uint32_t)m->line) { raise(m, MI_VI); m->vi_intrs++; }
        if (m->cpu.jit) jit_run(&m->cpu, per);       /* recompilateur */
        else vr4300_run(&m->cpu, per);
        ai_update(m);
        m->line += 2;
        if (m->line >= lines) {
            m->line = 0;
            m->frames++;
            n64_render(m);
            return 1;
        }
    }
    return 0;
}

void n64_render(N64 *m)
{
    uint32_t st = m->vi[0], type = st & 3, origin = m->vi[1] & 0xFFFFFF, width = m->vi[2] & 0xFFF;
    uint32_t hs = m->vi[9], vs = m->vi[10];
    int w, h, x, y;
    uint32_t xscale = m->vi[12] & 0xFFF, yscale = m->vi[13] & 0xFFF;
    int hw = (int)((hs & 0x3FF) - ((hs >> 16) & 0x3FF)), vh = (int)((vs & 0x3FF) - ((vs >> 16) & 0x3FF)) / 2;

    if (m->fs_skipped && m->fb_valid) { m->fs_skipped = 0; m->frames_skipped++; return; }   /* garde l'image precedente */
    m->fb_new = 1;
    m->fs_skipped = 0;
    m->fb_valid = 0;
    if (type < 2 || !width) return;
    w = (xscale && hw > 0) ? (int)((uint32_t)hw * xscale / 1024) : (int)width;
    h = (yscale && vh > 0) ? (int)((uint32_t)vh * yscale / 1024) : (int)(width * 3 / 4);
    if (w > (int)width) w = (int)width;
    if (w > N64_FB_MAXW) w = N64_FB_MAXW;
    if (h > N64_FB_MAXH) h = N64_FB_MAXH;
    if (w < 8 || h < 8) return;
    for (y = 0; y < h; y++) {
        uint32_t *out = m->fb + y * w;
        for (x = 0; x < w; x++) {
            if (type == 2) {
                uint32_t a = origin + ((uint32_t)y * width + (uint32_t)x) * 2;
                uint32_t p = a + 1 < m->rdram_size ? (uint32_t)(m->rdram[a] << 8 | m->rdram[a + 1]) : 0;
                uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
                out[x] = (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
            } else {
                uint32_t a = origin + ((uint32_t)y * width + (uint32_t)x) * 4;
                out[x] = a + 3 < m->rdram_size ? (be32(m->rdram + a) >> 8) : 0;
            }
        }
    }
    m->fb_w = w;
    m->fb_h = h;
    m->fb_valid = 1;
}

/* ------------------------------------------------------------ etats de sauvegarde
 * format : en-tete (magique, version, taille de la structure, taille RDRAM,
 * empreinte de la ROM) + structure N64 + RDRAM.  Un etat n'est valable que
 * pour la meme version de l'emulateur et la meme ROM. */

#define STATE_MAGIC "N64PS3ET"
#define STATE_VERSION 4u

static uint32_t rom_tag(const N64 *m) { return m->rom_size >= 0x18 ? be32(m->rom + 0x10) ^ be32(m->rom + 0x14) : 0; }

int n64_state_save(N64 *m, const char *path)
{
    FILE *f;
    uint32_t h[4];
    int ok;
    gfx_wait(m);
    f = fopen(path, "wb");
    if (!f) return -1;
    h[0] = STATE_VERSION; h[1] = (uint32_t)sizeof *m; h[2] = m->rdram_size; h[3] = rom_tag(m);
    ok = fwrite(STATE_MAGIC, 1, 8, f) == 8 && fwrite(h, sizeof h, 1, f) == 1 &&
         fwrite(m, sizeof *m, 1, f) == 1 && fwrite(m->rdram, 1, m->rdram_size, f) == m->rdram_size;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int n64_state_load(N64 *m, const char *path)
{
    FILE *f;
    char magic[8];
    uint32_t h[4];
    N64 *t;
    uint8_t *ram;
    gfx_wait(m);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, STATE_MAGIC, 8) || fread(h, sizeof h, 1, f) != 1 ||
        h[0] != STATE_VERSION || h[1] != (uint32_t)sizeof *m || h[2] != m->rdram_size || h[3] != rom_tag(m)) { fclose(f); return -2; }
    t = (N64 *)malloc(sizeof *t);
    ram = (uint8_t *)malloc(m->rdram_size);
    if (!t || !ram || fread(t, sizeof *t, 1, f) != 1 || fread(ram, 1, m->rdram_size, f) != m->rdram_size) {
        free(t); free(ram); fclose(f); return -3;
    }
    fclose(f);
    /* les pointeurs restent ceux de la session courante */
    t->rdram = m->rdram; t->rom = m->rom; t->rom_size = m->rom_size;
    t->cpu.rdram = m->rdram; t->cpu.rdram_size = m->rdram_size; t->cpu.ctx = m; t->cpu.read = m->cpu.read; t->cpu.write = m->cpu.write;
    t->cpu.jit = m->cpu.jit; t->cpu.codepage = m->cpu.codepage; t->cpu.code_written = m->cpu.code_written;
    t->cpu.jit_map = m->cpu.jit_map; t->cpu.jit_code_base = m->cpu.jit_code_base; t->cpu.jit_npages = m->cpu.jit_npages;
    t->rdp.rdram = m->rdram; t->rdp.zbuf = m->rdp.zbuf; t->rdp.zbuf_for = 0xFFFFFFFFu;
    t->rdp.tcache = m->rdp.tcache; t->rdp.tmem_gen++; memset(t->rdp.ce_sig, 0, sizeof t->rdp.ce_sig); t->rdp.fast = m->rdp.fast;
    t->gsp.rdram = m->rdram; t->gsp.rdp = &m->rdp;
    t->audio_out = m->audio_out; t->audio_ctx = m->audio_ctx; t->clock_us = m->clock_us;
    t->gfx_worker = m->gfx_worker;
    if (t->gfx_pending) t->gfx_waited = 1;           /* sauve apres la fin du dessin */
    memcpy(m, t, sizeof *m);
    memcpy(m->rdram, ram, m->rdram_size);
    if (m->cpu.jit) jit_flush(&m->cpu);                  /* le code compile ne correspond plus */
    free(t); free(ram);
    return 0;
}
