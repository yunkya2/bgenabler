/*
 * Copyright (c) 2026 Yuichi Nakamura (@yunkya2)
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

    .section    ".header","ax"

/* Human68k device driver header */

    .global devheader
    .global reqheader

devheader:
    .long   -1                  // link pointer
    .word   0x8000              // device type
    .long   strategy            // strategy routine entry point
    .long   interrupt_asm       // interrupt routine entry point
    .ascii  "BGENB*/-"          // device name
    .long   bgenabler_data      // device data pointer

reqheader:  .long 0

strategy:
    move.l  %a5,reqheader
    rts

interrupt_asm:
    movem.l %d0-%d2/%a0-%a2/%a5,%sp@-
    movea.l reqheader,%a5
    move.w  #0x700d,%d0             // Initialize以外はエラーにする
    tst.b   %a5@(2)
    bne     1f
    bsr     interrupt
1:
    move.b  %d0,%a5@(3)             // errl エラーコードを設定
    ror.w   #8,%d0
    move.b  %d0,%a5@(4)             // errh エラーコードを設定
    movem.l %sp@+,%d0-%d2/%a0-%a2/%a5
    rts

//****************************************************************************
// Timer-C割り込み処理ルーチン (バックグラウンドタスク処理も行う)
//****************************************************************************

timerc_intr:                            // Timer-C割り込み処理 (100Hz)
    tst.b   0x0cbc.w
    beq.s   1f
    clr.w   %sp@-                       // (68000以外の場合のスタックフレーム調整)
1:
    pea.l   %pc@(timerc_bg)             // 元のTimer-C割り込み処理が終わったら呼び出す処理
    move.w  %sr,%sp@-
    move.l  %pc@(old_timerc),%sp@-
    rts

timerc_bg:                              // Timer-Dの代わりのBG処理呼び出し
    movem.l %d0/%a0,%sp@-
    lea.l   0x00e790,%a0                // (Human68k v3.02の内部ワークエリア)
    moveq.l #10,%d0                     // 周期が1/10なのでカウント10回分処理
    add.l   %d0,%a0@(-8)                // Tick count増加 (DOS _TIME_PR用)
    sub.b   %d0,%a0@                    // タイムスライス残量を減算
    bls.s   1f
    movem.l %sp@+,%d0/%a0               // まだタイムスライスが残っている
    rte
1:                                      // スレッドがタイムスライスを使い果たした
    move.b  %a0@(1),%a0@                // タイムスライスカウンタを初期化
    movem.l %sp@+,%d0/%a0
    jmp     0x00e030                    // Human68k v3.02のスレッド切り替え処理へ

//****************************************************************************
// INTERRUPTスイッチ, COPYキー処理 (メインスレッドでなかったら保留する)
//****************************************************************************

nmi_intr:                               // NMI割り込み処理(INTERRUPT SW)
    move.l  %d0,%sp@-
    move.l  0x1c54.w,%d0
    beq.s   1f                          // バックグラウンド処理が有効でない
    cmp.l   0x1c50.w,%d0
    beq.s  1f                           // メインスレッド(スレッド0)を実行中
    move.l  %sp@+,%d0
    move.b  #0x0c,0xe8e007              // NMI発生をリセットする
    move.l  %pc@(old_nmivect),pendvect  // メインスレッドで実行するために一旦保留
    rte
1:                                      // ベクタ変更前のNMI割り込み処理へ
    move.l  %sp@+,%d0
    move.l  %pc@(old_nmivect),%sp@-
    rts

trap12_intr:                            // trap #12(COPYキー)割り込み処理
    move.l  %d0,%sp@-
    move.l  0x1c54.w,%d0
    beq.s   1f                          // バックグラウンド処理が有効でない
    cmp.l   0x1c50.w,%d0
    beq.s   1f                          // メインスレッド(スレッド0)を実行中
    move.l  %sp@+,%d0
    move.l  %pc@(old_trap12vect),pendvect // メインスレッドで実行するために一旦保留
trap12_intr_nop:
    rte
1:                                      // ベクタ変更前のtrap #12割り込み処理へ
    move.l  %sp@+,%d0
    move.l  %pc@(old_trap12vect),%sp@-
    rts

//****************************************************************************
// 保留中のINTERRUPTスイッチ, COPYキー処理をメインスレッドで呼び出す
//****************************************************************************

pendvect:
    .long   0                           // メインスレッドに戻るまで呼び出しを保留している割り込みベクタ

dos_change_pr:                          // Human68kのスレッド切り替え時に呼び出される
    move.l  %pc@(pendvect),%d0
    beq     1f                          // 保留している割り込みはない
    move.l  0x1c54.w,%d0
    beq.s   1f                          // バックグラウンド処理が有効でない
    cmp.l   0x1c50.w,%d0
    bne.s   1f                          // メインスレッド(スレッド0)以外に切り替わった

    // スレッド切り替え処理のNMIフラグを利用して保留している割り込みを呼ばせる
    lea.l   0x00e784,%a0                // (Human68k v3.02の内部ワークエリア)
    move.l  #dos_change_pr_nmi,%a0@     // スレッド切り替え終了後に呼び出される
    st      %a0@(-2)                    // NMIフラグをセットする
1:
    rts

dos_change_pr_nmi:
    move.l  #nmi_intr,0x007c.w          // NMI割り込みベクタを元に戻す
    move.l  %pc@(pendvect),%sp@-
    clr.l   pendvect                    // 保留している割り込みベクタをクリア
    rts                                 // 保留していた割り込みをメインスレッドで呼び出す


//****************************************************************************

    .align  4

//  bgenabler_data 構造体

    .global bgenabler_data
bgenabler_data:
    .long   0x42474501                  // "BGE\x01"

    // 常駐部のデータ、関数エントリ
    .long   thread_table        // スレッド管理構造体
    .long   timerc_intr         // Timer-C割り込み処理
    .long   nmi_intr            // INTERRUPTスイッチ割り込み処理
    .long   trap12_intr         // COPYキー割り込み処理
    .long   trap12_intr_nop     // COPYキー割り込み処理 (無効化する場合)
    .long   dos_change_pr       // Human68kのスレッド切り替え時に呼び出される

    // バックグラウンド機能パラメータ設定値
    .byte   32                  // maxthreads
    .byte   32                  // threads
    .byte   10                  // level
    .byte   10                  // slice
    .byte   0                   // opt_c
    .byte   0                   // opt_d
    .byte   0                   // opt_x
    .byte   0                   // opt_n

    .long   0                   // 常駐部のメモリブロック
old_nmivect:                    // NMI割り込みベクタの新旧設定値
    .long   0
    .long   0
old_trap12vect:                 // trap #12割り込みベクタの新旧設定値
    .long   0
    .long   0
old_timerc:                     // Timer-C割り込みベクタの新旧設定値
    .long   0
    .long   0
old_iocs_timerdst:              // IOCS _TIMERDSTの新旧設定値
    .long   0
    .long   0
old_dos_change_pr:              // DOS _CHANGE_PRの新旧設定値
    .long   0
    .long   0

    .global thread_table

thread_table:
    .space  0x78 * 32           // Human68kスレッド管理構造体

    .end
