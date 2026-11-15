; Headless guest test for real emulated CPU -> UART -> host TCP -> echo.
; BSD-2-Clause. Assemble with 64tass -a -B --nostart.
.cpu "65c02"
.enc "ascii"
.cdef 0,127,0
.weak
USING_LIBRARY = 0
.endweak
*=$0800
start:
    stz $9fb4 ; disable automatic RAM dumps in this test
    cld
.if USING_LIBRARY
    lda #4
    ldx #$e0
    ldy #$9f
    jsr $8000
    bcs fail
    jsr $8018
    bcs fail
    ldx #<endpoint
    ldy #>endpoint
    jsr $801b
    bcs fail
.else
    lda #$80
    sta $9fe3
    lda #8
    sta $9fe0
    stz $9fe1
    lda #3
    sta $9fe3
    stz $9fe1
    lda #$87
    sta $9fe2
    lda #$27
    sta $9fe4
    ldx #<profile
    ldy #>profile
    jsr send_string
    ; Drain startup/echo through exact OK CR sequence.
    stz match_index
response:
    jsr get_byte
    ldx match_index
    cmp ok_text,x
    bne mismatch
    inc match_index
    lda match_index
    cmp #3
    bne response
    bra dial
mismatch:
    stz match_index
    bra response
dial:
    ldx #<dial_prefix
    ldy #>dial_prefix
    jsr send_string
    ldx #<endpoint
    ldy #>endpoint
    jsr send_string
    ldx #<dial_suffix
    ldy #>dial_suffix
    jsr send_string
    jsr budget_start
carrier:
    jsr budget_tick
    lda $9fe6
    and #$80
    beq carrier
.endif
    ldx #<connected_text
    ldy #>connected_text
    jsr print
    stz value
    lda #16
    sta pages
echo_loop:
    lda value
    jsr put_byte
    jsr get_byte
    cmp value
    bne fail
    inc value
    bne echo_loop
    dec pages
    bne echo_loop
    ldx #<transfer_text
    ldy #>transfer_text
    jsr print
.if USING_LIBRARY
    jsr $801e
    bcs fail
.else
    lda #$26
    sta $9fe4
    jsr budget_start
disconnected:
    jsr budget_tick
    lda $9fe6
    and #$80
    bne disconnected
.endif
    ldx #<done_text
    ldy #>done_text
    jsr print
    jmp $ffff

put_byte:
    sta outgoing
    jsr budget_start
tx_wait:
    jsr budget_tick
.if USING_LIBRARY
    lda outgoing
    jsr $8009
    bcc put_done
    cmp #1
    bne fail
.else
    lda $9fe5
    and #$20
    bne tx_ready
.endif
    bra tx_wait
.if !USING_LIBRARY
tx_ready:
    lda outgoing
    sta $9fe0
.endif
put_done:
    rts
get_byte:
    jsr budget_start
rx_wait:
    jsr budget_tick
.if USING_LIBRARY
    jsr $8006
    bcc get_done
    cmp #1
    bne fail
.else
    lda $9fe5
    and #1
    bne rx_ready
.endif
    bra rx_wait
.if !USING_LIBRARY
rx_ready:
    lda $9fe0
.endif
get_done:
    rts
budget_start:
    stz budget
    stz budget+1
    lda #$40
    sta budget+2
    rts
budget_tick:
    lda budget
    bne dec_low
    lda budget+1
    bne dec_mid
    lda budget+2
    beq fail
    dec budget+2
dec_mid:
    dec budget+1
dec_low:
    dec budget
    rts
send_string:
    stx send_load+1
    sty send_load+2
send_more:
send_load:
    lda $ffff
    beq send_done
    jsr put_byte
    inc send_load+1
    bne send_more
    inc send_load+2
    bra send_more
send_done:
    rts
print:
    stx print_load+1
    sty print_load+2
print_more:
print_load:
    lda $ffff
    beq print_done
    sta $9fbb
    inc print_load+1
    bne print_more
    inc print_load+2
    bra print_more
print_done:
    rts
fail:
    sta $9fb9
    ldx #<failure_text
    ldy #>failure_text
    jsr print
    jmp $ffff
profile: .text "ATE0Q0V1F0R0&P0S62=0S63=2N0H"
    .byte 13,0
ok_text: .text "OK"
    .byte 13
dial_prefix: .text 'ATQ1D"'
    .byte 0
endpoint: .text "127.0.0.1:00000"
    .byte 0
dial_suffix: .byte '"',13,0
connected_text: .text "ok 1 - guest connects through emulated card"
    .byte 10,0
transfer_text: .text "ok 2 - 4096 raw echo bytes match"
    .byte 10,0
done_text: .text "ok 3 - DTR disconnect"
    .byte 10
    .text "1..3"
    .byte 10,0
failure_text: .text "not ok - guest transport failure"
    .byte 10,0
value: .byte 0
pages: .byte 0
outgoing: .byte 0
match_index: .byte 0
budget: .fill 3,0
