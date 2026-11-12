# demo.s : ROM de demonstration maison (code original, libre de droits).
# Programme l'interface video en 320x240 16 bits, lit la manette 1 via le PIF
# et deplace un carre. A change sa couleur. Le fond defile en degrade.

FB      = 0xA0100000        # image (non cachee) : 320 x 240 x 2 octets
PIFBUF  = 0xA0080000        # bloc de commandes PIF
VI      = 0xA4400000
SI      = 0xA4800000

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

        li      s0, 152                 # x du carre
        li      s1, 112                 # y du carre
        li      s2, 0                   # compteur de trames
        li      s3, 0xF801              # couleur (rouge)

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
        lb      t5, 7(t0)               # stick Y

        sra     t6, t4, 4
        addu    s0, s0, t6
        sra     t6, t5, 4
        subu    s1, s1, t6
        andi    t6, t3, 0x0800          # croix haut
        beq     t6, zero, n_up
        nop
        addiu   s1, s1, -2
n_up:   andi    t6, t3, 0x0400
        beq     t6, zero, n_dn
        nop
        addiu   s1, s1, 2
n_dn:   andi    t6, t3, 0x0200
        beq     t6, zero, n_lt
        nop
        addiu   s0, s0, -2
n_lt:   andi    t6, t3, 0x0100
        beq     t6, zero, n_rt
        nop
        addiu   s0, s0, 2
n_rt:   andi    t6, t3, 0x8000          # bouton A : nouvelle couleur
        beq     t6, zero, n_a
        nop
        sll     s3, s2, 6
        ori     s3, s3, 0xF801
n_a:
        # bornes : 0 <= x <= 304, 0 <= y <= 224
        bgez    s0, cx1
        nop
        li      s0, 0
cx1:    slti    t6, s0, 305
        bne     t6, zero, cx2
        nop
        li      s0, 304
cx2:    bgez    s1, cy1
        nop
        li      s1, 0
cy1:    slti    t6, s1, 225
        bne     t6, zero, cy2
        nop
        li      s1, 224
cy2:
        # ---- fond en degrade defilant
        li      t0, FB
        li      t7, 0                   # y
row:    li      t8, 0                   # x
        srl     t9, t7, 3
        andi    t9, t9, 31
        sll     t9, t9, 1               # bleu = y / 8
col:    addu    t1, t8, s2
        srl     t1, t1, 3
        andi    t1, t1, 31
        sll     t1, t1, 6               # vert = (x + trame) / 8
        or      t1, t1, t9
        ori     t1, t1, 1
        sh      t1, 0(t0)
        addiu   t0, t0, 2
        addiu   t8, t8, 1
        slti    t2, t8, 320
        bne     t2, zero, col
        nop
        addiu   t7, t7, 1
        slti    t2, t7, 240
        bne     t2, zero, row
        nop

        # ---- carre 16 x 16
        li      t1, 640
        mult    s1, t1
        mflo    t0
        sll     t1, s0, 1
        addu    t0, t0, t1
        li      t1, FB
        addu    t0, t0, t1
        li      t7, 16
sq_r:   li      t8, 16
        move    t2, t0
sq_c:   sh      s3, 0(t2)
        addiu   t2, t2, 2
        addiu   t8, t8, -1
        bne     t8, zero, sq_c
        nop
        addiu   t0, t0, 640
        addiu   t7, t7, -1
        bne     t7, zero, sq_r
        nop

        addiu   s2, s2, 1

        # ---- attente du retour de trame (ligne VI >= 480 puis < 480)
        li      t0, VI
w1:     lw      t1, 16(t0)
        slti    t2, t1, 480
        bne     t2, zero, w1
        nop
w2:     lw      t1, 16(t0)
        slti    t2, t1, 480
        beq     t2, zero, w2
        nop
        j       main
        nop
