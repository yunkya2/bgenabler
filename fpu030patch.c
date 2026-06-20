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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <x68k/dos.h>

//****************************************************************************
// Patch data for 68030 FPU error $000d workaround
//****************************************************************************

// $00e05a - $00e066
// タスク切り替え処理開始の修正
// 積まれたスタックフレームのボトムアドレスを求める処理を削除
// (スタック上に積んだままにするので不要)
static uint16_t hupatch1[] = { 0x600a, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };

// $00e0a6 - $00e0b4
// 切り替え元タスクのコンテキスト保存処理の修正
// 例外スタックフレームの先頭アドレスをSSPの保存値とする
static uint16_t hupatch2[] = { 0x598d, 0x4def, 0x000e, 0x2b26, 0x3b26, 0x2b4e, 0x0006 };

// $00e16c - $00e17a
// 切り替え先タスクのコンテキスト復帰処理の修正
// タスク復帰の際のスタックフレームを再構築せず、タスク切り替え前に積まれたフレームをそのまま使う
static uint16_t hupatch3[] = { 0x2f66, 0x0002, 0x3ea6, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };

// $00e5bc - $00e5e8
// DOS _OPEN_PRの修正
// 新規作成するタスクのスーパバイザスタックにエントリポイントに飛ぶための例外スタックフレームを
// あらかじめ積んでおく
static uint16_t hupatch4[] = { 0x700c, 0x429d, 0x51c8, 0xfffc, 0x429d, 0x429d, 0x2856, 0x4a38,
                               0x0cbc, 0x6702, 0x4264, 0x302e, 0x0004, 0x0240, 0x201f, 0x3ac0,
                               0x222e, 0x0006, 0x2901, 0x2ac1, 0x3900, 0x2acc };

//****************************************************************************
// Patch data for 68030 FPU error $000d workaround
//****************************************************************************

// Human68k 3.02に68030 FPU命令使用時のエラー$000d対策パッチを当てる
void fpu030patch(void)
{
    if (memcmp((void *)0x00e05a, hupatch1, sizeof(hupatch1)) == 0) {
        // 既にパッチ済み
        return;
    }

    if (*(void **)0x1c54 != NULL) {
        // BGタスク有効時はパッチ当てしない
        return;
    }

    if (*(uint8_t *)0x000cbc > 0x01) {
        // 68020以上のCPUならIOCS _SYS_STATでキャッシュフラッシュする
        __asm__ volatile (
            "moveq.l #3,%%d1\n"
            "moveq.l #0xffffffac,%%d0\n"
            "trap    #15"
            : : : "d0", "d1", "memory"
        );
    }

    // Human68kにパッチを当てる
    memcpy((void *)0x00e05a, hupatch1, sizeof(hupatch1));   // タスク切り替え処理開始
    memcpy((void *)0x00e0a6, hupatch2, sizeof(hupatch2));   // 切り替え元タスクのコンテキスト保存処理
    memcpy((void *)0x00e16c, hupatch3, sizeof(hupatch3));   // 切り替え先タスクのコンテキスト復帰処理
    memcpy((void *)0x00e5bc, hupatch4, sizeof(hupatch4));   // DOS _OPEN_PR

    _dos_print("Human68kのタスク切り替え処理にパッチを適用しました\r\n");
}
