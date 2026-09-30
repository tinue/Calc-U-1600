                              1 ; BUSROM.ASM -- a tiny ROM for the debugger's bus-ROM smoke test
                              2 ; (tools/dap_smoke.py): plugged in at &8000 by a preset's `bus-rom:`,
                              3 ; entered with CALL &8000, stops at BUSHIT when a breakpoint is set there.
                              4 ; Also the listing test for sdaslh5801's eight-digit addresses: code and
                              5 ; equates of 8000H and up print as FFFF8000 (listing_tests.cpp).
                              6 ;
                              7 ; Assembly command (as the VS Code sdas task does):
                              8 ;   sdaslh5801 -plosgff busrom.asm, sdld, makebin -s 65536 -> busrom.bin + busrom.rst
                              9 
                     FFFF9000    10 SCRATCH     .equ    0x9000
                             11 
                             12             .area   CODE (ABS)
   FFFF8000                      13             .org    0x8000
                             14 
   FFFF8000 38                   15 BUSROM:     nop
   FFFF8001 38                   16             nop
   FFFF8002 A5 90 00             17 BUSHIT:     lda     (SCRATCH)
   FFFF8005 9A                   18             rtn
   FFFF8006 01 02 03 04 05 06    19 TABLE:      .db     1,2,3,4,5,6,7,8
        07 08
