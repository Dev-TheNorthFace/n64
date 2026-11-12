# demo.s : ROM de demonstration maison (code original, libre de droits).
# Etape 3 : cube 3D texture tournant, dessine par le microcode graphique
# (liste d'affichage F3DEX2 envoyee au RSP), double tampon, tampon Z.
# Stick / croix gauche-droite : vitesse de rotation.  A : sens inverse.
# Etape 4 : B maintenu = son (liste audio ABI1 envoyee au RSP puis DMA AI).
# Remarque : sur une vraie N64 il faudrait aussi charger le microcode dans
# le RSP ; ici l'emulateur l'execute en haut niveau (HLE).
# Les donnees (matrices, sommets, texture, liste) sont generees par make_demo.py.

PIFBUF  = 0xA0080000        # bloc de commandes PIF
VI      = 0xA4400000
SI      = 0xA4800000
SPSTAT  = 0xA4040010
TASK    = 0xA4000FC0
MIMODE  = 0xA4300000
AIREG   = 0xA4500000

start:
        lui     sp, 0x8020
        li      t0, VI
        li      t1, 0x0000320E          # 16 bits, anti-crenelage, gamma
        sw      t1, 0(t0)
        li      t1, 0x00100000          # origine de l'image (adresse physique)
        sw      t1, 4(t0)
        li      t1, 320                 # largeur
        sw      t1, 8(t0)
        li      t1, 2                   # interruption VI
        sw      t1, 12(t0)
        li      t1, 0x0404233A
        sw      t1, 20(t0)
        li      t1, 525                 # V_SYNC (NTSC)
        sw      t1, 24(t0)
        li      t1, 0x00000C15
        sw      t1, 28(t0)
        li      t1, 0x0C150C15
        sw      t1, 32(t0)
        li      t1, 0x006C02EC          # H_START
        sw      t1, 36(t0)
        li      t1, 0x002501FF          # V_START
        sw      t1, 40(t0)
        li      t1, 0x000E0204
        sw      t1, 44(t0)
        li      t1, 0x00000200          # X_SCALE (320 / 640)
        sw      t1, 48(t0)
        li      t1, 0x00000400          # Y_SCALE
        sw      t1, 52(t0)

        li      s2, 0                   # angle (index dans la table de matrices)
        li      s4, 0                   # tampon courant
        li      s5, 1                   # sens de rotation
        li      s7, 0                   # bouton A precedent

main:
        # ---- lecture de la manette 1 : FF 01 04 01 | 4 octets de reponse | FE
        li      t0, PIFBUF
        li      t1, 0xFF010401
        sw      t1, 0(t0)
        li      t1, 0xFFFFFFFF
        sw      t1, 4(t0)
        li      t1, 0xFE000000
        sw      t1, 8(t0)
        addiu   t2, t0, 12
        addiu   t3, t0, 60
clr:    sw      zero, 0(t2)
        bne     t2, t3, clr
        addiu   t2, t2, 4
        li      t1, 1
        sw      t1, 60(t0)
        li      t2, SI
        li      t1, 0x00080000
        sw      t1, 0(t2)               # SI_DRAM_ADDR
        li      t1, 0x1FC007C0
        sw      t1, 16(t2)              # RDRAM -> PIF
        li      t1, 0x00080000
        sw      t1, 0(t2)
        li      t1, 0x1FC007C0
        sw      t1, 4(t2)               # PIF -> RDRAM
        sw      zero, 24(t2)            # acquitte l'interruption SI
        lhu     t3, 4(t0)               # boutons
        lb      t4, 6(t0)               # stick X

        # ---- A (front montant) : inverse le sens
        andi    t6, t3, 0x8000
        beq     t6, s7, n_a
        move    s7, t6
        beq     t6, zero, n_a
        nop
        subu    s5, zero, s5
n_a:
        # ---- vitesse = sens + stick / 16 + croix
        sra     t6, t4, 4
        addu    t6, t6, s5
        andi    t7, t3, 0x0200          # gauche
        beq     t7, zero, n_lt
        nop
        addiu   t6, t6, -2
n_lt:   andi    t7, t3, 0x0100          # droite
        beq     t7, zero, n_rt
        nop
        addiu   t6, t6, 2
n_rt:   addu    s2, s2, t6

        # ---- matrice de la trame -> liste d'affichage
        andi    t0, s2, 127
        sll     t0, t0, 6
        li      t1, MTXTAB
        addu    t0, t0, t1
        li      t1, DLMV + 0x20000000
        sw      t0, 4(t1)

        # ---- tampon de dessin : 0x100000 ou 0x140000
        xori    s4, s4, 1
        sll     t0, s4, 18
        li      t1, 0x00100000
        addu    s6, t0, t1
        li      t1, DLCIMG + 0x20000000
        sw      s6, 4(t1)

        # ---- tache graphique (OSTask en DMEM) puis demarrage du RSP
        li      t0, TASK
        li      t1, 1
        sw      t1, 0(t0)               # type = graphismes
        li      t1, UCSTR
        sw      t1, 24(t0)              # ucode_data
        li      t1, 64
        sw      t1, 28(t0)              # ucode_data_size
        li      t1, DL
        sw      t1, 48(t0)              # data_ptr = liste d'affichage
        li      t0, SPSTAT
        li      t1, 5                   # efface HALT et BROKE : le RSP demarre
        sw      t1, 0(t0)
wsp:    lw      t1, 0(t0)               # attend la fin (HALT)
        andi    t1, t1, 1
        beq     t1, zero, wsp
        nop
        li      t1, 8
        sw      t1, 0(t0)               # acquitte l'interruption SP
        li      t0, MIMODE
        li      t1, 0x800
        sw      t1, 0(t0)               # acquitte l'interruption DP

        # ---- son : volume selon B, tampon de sortie, tache audio, DMA AI
        andi    t6, t3, 0x4000          # bouton B
        li      t7, 0
        beq     t6, zero, n_b
        nop
        li      t7, 0x3000
n_b:    li      t0, AVOL + 0x20000000
        lui     t1, 0x0906
        or      t1, t1, t7
        sw      t1, 0(t0)               # volume gauche
        lui     t1, 0x0904
        or      t1, t1, t7
        sw      t1, 8(t0)               # volume droit
        lui     t1, 0x0902
        or      t1, t1, t7
        sw      t1, 16(t0)              # cible gauche
        lui     t1, 0x0900
        or      t1, t1, t7
        sw      t1, 24(t0)              # cible droite
        sll     t1, s4, 12
        li      t2, 0x011C0000          # segment 1 = tampon de sortie
        addu    t1, t1, t2
        li      t0, ASEG + 0x20000000
        sw      t1, 4(t0)
        li      t0, TASK
        li      t1, 2
        sw      t1, 0(t0)               # type = son
        li      t1, AUCODE
        sw      t1, 24(t0)
        li      t1, ALIST
        sw      t1, 48(t0)
        li      t1, ALISTEND - ALIST
        sw      t1, 52(t0)
        li      t0, SPSTAT
        li      t1, 5
        sw      t1, 0(t0)
wsp2:   lw      t1, 0(t0)
        andi    t1, t1, 1
        beq     t1, zero, wsp2
        nop
        li      t1, 8
        sw      t1, 0(t0)
        li      t0, AIREG
        li      t1, 1535                # 48 681 812 / 1536 = 31 694 Hz
        sw      t1, 16(t0)
        li      t1, 15
        sw      t1, 20(t0)
        li      t1, 1
        sw      t1, 8(t0)
        sll     t1, s4, 12
        li      t2, 0x001C0000
        addu    t1, t1, t2
        sw      t1, 0(t0)               # AI_DRAM_ADDR
        li      t1, 2112                # 528 echantillons stereo
        sw      t1, 4(t0)               # AI_LEN : lance le DMA

        # ---- attente du retour de trame puis affichage du tampon dessine
        li      t0, VI
w1:     lw      t1, 16(t0)
        slti    t2, t1, 480
        bne     t2, zero, w1
        nop
w2:     lw      t1, 16(t0)
        slti    t2, t1, 480
        beq     t2, zero, w2
        nop
        sw      s6, 4(t0)               # VI_ORIGIN
        j       main
        nop

        .align  8
