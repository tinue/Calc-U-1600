; BUSROM.ASM -- a tiny ROM for the debugger's bus-ROM smoke test
; (tools/dap_smoke.py): plugged in at &8000 by a preset's `bus-rom:`,
; entered with CALL &8000, stops at BUSHIT when a breakpoint is set there.
; Also the listing test for sdaslh5801's eight-digit addresses: code and
; equates of 8000H and up print as FFFF8000 (listing_tests.cpp).
;
; Assembly command (as the VS Code sdas task does):
;   sdaslh5801 -plosgff busrom.asm, sdld, makebin -s 65536 -> busrom.bin + busrom.rst

SCRATCH     .equ    0x9000

            .area   CODE (ABS)
            .org    0x8000

BUSROM:     nop
            nop
BUSHIT:     lda     (SCRATCH)
            rtn
TABLE:      .db     1,2,3,4,5,6,7,8
