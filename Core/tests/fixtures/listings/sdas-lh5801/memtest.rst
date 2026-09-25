                              1 ; ============================================================
                              2 ; MEMTEST.ASM -- Generic Free-Memory Test for Sharp PC-1500 / PC-1500A
                              3 ; ============================================================
                              4 ;
                              5 ; Tests all currently-free, currently-installed RAM for faults: writes
                              6 ; four patterns (0x55, 0xAA, 0xFF, 0x00) to every byte from BASIC's own
                              7 ; free-memory pointer (BOTTOM_H/L, &7867/&7868) up to the ROM's own
                              8 ; end-of-RAM pointer (RAM_END_H, &7864), reading each back and comparing.
                              9 ; At the first mismatch the bad address and data are saved and the test
                             10 ; stops; on a clean pass ERR_FLAG is 0. This clobbers any DIMmed
                             11 ; variables/BASIC program in that range -- by design, not a bug.
                             12 ;
                             13 ; Because it always uses the machine's own reported free-RAM bounds, one
                             14 ; binary works correctly whether a memory module is installed or not,
                             15 ; and whatever its size -- the only thing that has to match the real
                             16 ; hardware is where THIS CODE ITSELF gets loaded, since that's baked into
                             17 ; the binary as absolute addresses (LH5801 code isn't
                             18 ; position-independent). That address follows the usual convention:
                             19 ; RAM_start + &C5 (the ROM reserves the first &C5 bytes of any RAM block
                             20 ; for its own use) -- see "Build selection" below.
                             21 ;
                             22 ; Calling convention: X = iteration count on entry (1-255; 0 = skipped,
                             23 ; ERR_FLAG=2). Returns with carry set, X = ERR_FLAG (0 = pass, 1 = fault,
                             24 ; 2 = not run). ERR_FLAG is at ENTRY+2; on a fault, ERR_ADDR_H/L,
                             25 ; ERR_EXPCT, ERR_ACTUAL (ENTRY+3..+6) hold the bad address and the
                             26 ; expected/actual byte.
                             27 ;
                             28 ; Assembly command:
                             29 ;   sdaslh5801 -plosgff memtest.asm
                             30 ;   (or use the lh5801-asm VS Code extension's "LH5801: Assemble to BIN")
                             31 ;
                             32 ; --- Build selection: uncomment ONE ENTRY line, assemble, then PEEK
                             33 ;     ENTRY+2..ENTRY+6 to read results ---
                             34 ;
                             35 ; PC-1500A -- machine language area, always present regardless of any
                             36 ; module (see examples/memtest_1500a.pc1500a):
                             37 ;   ENTRY = 0x7C01   NEW: none needed     RUN: CALL &7C01,X
                             38 ;
                             39 ; PC-1500 stock, no module -- built-in 2K RAM starts at &4000 (see
                             40 ; examples/memtest_stock.pc1500):
                             41 ;   ENTRY = 0x40C5   NEW &417D            RUN: CALL &40C5,X
                             42 ;
                             43 ; PC-1500 + CE-155 -- clamps around the built-in 2K (2K before it, 6K
                             44 ; after), giving one contiguous &3800-&5FFF block (10K):
                             45 ;   ENTRY = 0x38C5   NEW &397D            RUN: CALL &38C5,X
                             46 ;
                             47 ; PC-1500 + a 16K Y0 module -- &0000-&3FFF, contiguous with the built-in
                             48 ; 2K that follows it at &4000-&47FF (18K total):
                             49 ;   ENTRY = 0x00C5   NEW &017D            RUN: CALL &C5,X
                             50 ;
                             51 ; NEW's value above is ENTRY + this build's assembled size (184 bytes for
                             52 ; all four -- ENTRY doesn't change the code's own length), i.e. the exact
                             53 ; first free byte after the loaded stub, not a rounded-up guess.
                             54 ;
                             55 ; ============================================================
                             56 
                             57 ; --- Build selection: uncomment ONE line ---
                     40C5    58 ENTRY       .equ    0x40C5      ; PC-1500 stock, no module
                             59 ;ENTRY      .equ    0x7C01      ; PC-1500A
                             60 ;ENTRY      .equ    0x38C5      ; PC-1500 + CE-155
                             61 ;ENTRY      .equ    0x00C5      ; PC-1500 + 16K Y0 module
                             62 
                     7867    63 BOTTOM_H    .equ    0x7867      ; High byte of BASIC's own free-memory pointer
                     7868    64 BOTTOM_L    .equ    0x7868      ; Low byte  of BASIC's own free-memory pointer
                     7864    65 RAM_END_H   .equ    0x7864      ; High byte of first invalid page (one past top of RAM)
                             66 
                             67 ; ============================================================
                             68             .area   CODE (ABS)
   40C5                      69             .org    ENTRY
                             70 ; ============================================================
                             71 
                             72 ; +0  Two-byte forward branch; lands at MEMTEST (+7), skipping
                             73 ;     the five result bytes.
   40C5 8E 05                74             bch     MEMTEST     ; 8E 05
                             75 
                             76 ; +2  Result area -- read these with PEEK() after CALL
   40C7 00                   77 ERR_FLAG:   .db     0x00        ; 0 = all passes clean, 1 = fault found, 2 = not run
   40C8 00                   78 ERR_ADDR_H: .db     0x00        ; High byte of first bad address
   40C9 00                   79 ERR_ADDR_L: .db     0x00        ; Low byte  of first bad address
   40CA 00                   80 ERR_EXPCT:  .db     0x00        ; Pattern written   (expected value)
   40CB 00                   81 ERR_ACTUAL: .db     0x00        ; Value read back   (actual value)
                             82 
                             83 ; ============================================================
                             84 ; MEMTEST -- main test routine, entered via CALL from BASIC
                             85 ; ============================================================
   40CC                      86 MEMTEST:                        ; ENTRY+7
                             87 
                             88 ; Initialise: clear ERR_FLAG.
   40CC B5 00                89             ldi     a,0x00
   40CE AE 40 C7             90             sta     (ERR_FLAG)
                             91 
                             92 ; Check whether the iteration count (X register on entry) is zero.
                             93 ; CPA compares A with a register; A is still 0x00 from the LDI above.
   40D1 86                   94             cpa     xh          ; flags = 0x00 - xh
   40D2 89 0E                95             bzr     COUNT_OK    ; xh != 0 -> count is nonzero, proceed
   40D4 06                   96             cpa     xl          ; xh==0, flags = 0x00 - xl
   40D5 89 0B                97             bzr     COUNT_OK    ; xl != 0 -> count is nonzero, proceed
                             98 
                             99 ; Count is zero -- mark as "not run" and return.
   40D7 B5 02               100             ldi     a,0x02
   40D9 AE 40 C7            101             sta     (ERR_FLAG)
   40DC 48 00               102             ldi     xh,0x00
   40DE 4A 02               103             ldi     xl,0x02
   40E0 FB                  104             sec                 ; carry set -> BASIC writes X register back to variable
   40E1 9A                  105             rtn
                            106 
   40E2                     107 COUNT_OK:
                            108 ; Save the iteration count into UL, biased down by one: LOP branches
                            109 ; back unless UL was already 0 *before* its decrement, so priming it
                            110 ; with (count-1) makes the OUTER loop below run exactly `count` times.
                            111 ; Supported range: 1-255 (XH is not used for the count).
   40E2 04                  112             lda     xl
   40E3 2A                  113             sta     ul
   40E4 62                  114             dec     ul
                            115 
                            116 ; Load Y with the end sentinel: one past the last valid byte, read from
                            117 ; the ROM's own RAM_END_H pointer.
   40E5 A5 78 64            118             lda     (RAM_END_H)
   40E8 18                  119             sta     yh
   40E9 5A 00               120             ldi     yl,0x00
                            121 
                            122 ; ============================================================
                            123 ; OUTER -- one full sweep of all four patterns
                            124 ; ============================================================
   40EB                     125 OUTER:
                            126 ; ============================================================
                            127 ; Pass 1 -- pattern 0x55 (01010101)
                            128 ; ============================================================
   40EB                     129 PASS1:
   40EB A5 78 67            130             lda     (BOTTOM_H)          ; X = start of free memory
   40EE 08                  131             sta     xh
   40EF A5 78 68            132             lda     (BOTTOM_L)
   40F2 0A                  133             sta     xl
   40F3 B5 55               134             ldi     a,0x55
   40F5 28                  135             sta     uh                  ; UH holds current pattern
   40F6 A4                  136 LOOP1:      lda     uh
   40F7 0E                  137             sta     (x)                 ; write pattern
   40F8 05                  138             lda     (x)                 ; read back
   40F9 A6                  139             cpa     uh                  ; compare (flags = a - uh)
   40FA 89 67               140             bzr     ERROR               ; Z=0 -> mismatch -> record & stop
                            141 
   40FC 44                  142             inc     x                   ; advance pointer
                            143 
   40FD 84                  144             lda     xh                  ; 16-bit end check: X vs Y
   40FE 96                  145             cpa     yh                  ; flags = xh - yh
   40FF 91 0B               146             bcr     LOOP1               ; C=0: xh < yh -> keep going
   4101 89 04               147             bzr     NEXT1               ; C=1 and Z=0: xh > yh -> pass done (skip xl test)
   4103 04                  148             lda     xl
   4104 16                  149             cpa     yl                  ; flags = xl - yl
   4105 91 11               150             bcr     LOOP1               ; C=0: xl < yl -> keep going
   4107                     151 NEXT1:
                            152 ; ============================================================
                            153 ; Pass 2 -- pattern 0xAA (10101010)
                            154 ; ============================================================
   4107                     155 PASS2:
   4107 A5 78 67            156             lda     (BOTTOM_H)
   410A 08                  157             sta     xh
   410B A5 78 68            158             lda     (BOTTOM_L)
   410E 0A                  159             sta     xl
   410F B5 AA               160             ldi     a,0xAA
   4111 28                  161             sta     uh
                            162 
   4112 A4                  163 LOOP2:      lda     uh
   4113 0E                  164             sta     (x)
   4114 05                  165             lda     (x)
   4115 A6                  166             cpa     uh
   4116 89 4B               167             bzr     ERROR
                            168 
   4118 44                  169             inc     x
                            170 
   4119 84                  171             lda     xh
   411A 96                  172             cpa     yh
   411B 91 0B               173             bcr     LOOP2
   411D 89 04               174             bzr     NEXT2
   411F 04                  175             lda     xl
   4120 16                  176             cpa     yl
   4121 91 11               177             bcr     LOOP2
   4123                     178 NEXT2:
                            179 
                            180 ; ============================================================
                            181 ; Pass 3 -- pattern 0xFF (11111111)
                            182 ; ============================================================
   4123                     183 PASS3:
   4123 A5 78 67            184             lda     (BOTTOM_H)
   4126 08                  185             sta     xh
   4127 A5 78 68            186             lda     (BOTTOM_L)
   412A 0A                  187             sta     xl
   412B B5 FF               188             ldi     a,0xFF
   412D 28                  189             sta     uh
                            190 
   412E A4                  191 LOOP3:      lda     uh
   412F 0E                  192             sta     (x)
   4130 05                  193             lda     (x)
   4131 A6                  194             cpa     uh
   4132 89 2F               195             bzr     ERROR
                            196 
   4134 44                  197             inc     x
                            198 
   4135 84                  199             lda     xh
   4136 96                  200             cpa     yh
   4137 91 0B               201             bcr     LOOP3
   4139 89 04               202             bzr     NEXT3
   413B 04                  203             lda     xl
   413C 16                  204             cpa     yl
   413D 91 11               205             bcr     LOOP3
   413F                     206 NEXT3:
                            207 
                            208 ; ============================================================
                            209 ; Pass 4 -- pattern 0x00 (00000000)
                            210 ; ============================================================
   413F                     211 PASS4:
   413F A5 78 67            212             lda     (BOTTOM_H)
   4142 08                  213             sta     xh
   4143 A5 78 68            214             lda     (BOTTOM_L)
   4146 0A                  215             sta     xl
   4147 B5 00               216             ldi     a,0x00
   4149 28                  217             sta     uh
                            218 
   414A A4                  219 LOOP4:      lda     uh
   414B 0E                  220             sta     (x)
   414C 05                  221             lda     (x)
   414D A6                  222             cpa     uh
   414E 89 13               223             bzr     ERROR
                            224 
   4150 44                  225             inc     x
                            226 
   4151 84                  227             lda     xh
   4152 96                  228             cpa     yh
   4153 91 0B               229             bcr     LOOP4
   4155 89 04               230             bzr     NEXT4
   4157 04                  231             lda     xl
   4158 16                  232             cpa     yl
   4159 91 11               233             bcr     LOOP4
   415B                     234 NEXT4:
                            235 ; All four passes completed without error.
                            236 ; LOP decrements UL and branches back to OUTER unless UL was already 0
                            237 ; (see the priming comment at COUNT_OK above).
   415B 88 72               238             lop     ul,OUTER
                            239 
                            240 ; ============================================================
                            241 ; All iterations completed without error.
                            242 ; ============================================================
   415D 48 00               243             ldi     xh,0x00
   415F 4A 00               244             ldi     xl,0x00
   4161 FB                  245             sec                         ; carry set -> BASIC writes result to variable
   4162 9A                  246             rtn
                            247 
                            248 ; ============================================================
                            249 ; ERROR -- record the first mismatch and return to BASIC
                            250 ; ============================================================
                            251 ; On entry: A = value actually read, X = bad address, UH = pattern written.
   4163                     252 ERROR:
   4163 AE 40 CB            253             sta     (ERR_ACTUAL)        ; save the bad readback
   4166 A4                  254             lda     uh
   4167 AE 40 CA            255             sta     (ERR_EXPCT)         ; save the expected pattern
   416A 84                  256             lda     xh
   416B AE 40 C8            257             sta     (ERR_ADDR_H)        ; save bad-address high byte
   416E 04                  258             lda     xl
   416F AE 40 C9            259             sta     (ERR_ADDR_L)        ; save bad-address low byte
   4172 B5 01               260             ldi     a,0x01
   4174 AE 40 C7            261             sta     (ERR_FLAG)          ; mark failure
   4177 48 00               262             ldi     xh,0x00
   4179 4A 01               263             ldi     xl,0x01
   417B FB                  264             sec                         ; carry set -> BASIC writes result to variable
   417C 9A                  265             rtn
