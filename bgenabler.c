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

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <x68k/dos.h>
#include <x68k/iocs.h>

//****************************************************************************
// Macros and definitions
//****************************************************************************

struct dos_reqheader {
  uint8_t magic;       // +0x00.b  Constant (26)
  uint8_t unit;        // +0x01.b  Unit number
  uint8_t command;     // +0x02.b  Command code
  uint8_t errl;        // +0x03.b  Error code low
  uint8_t errh;        // +0x04.b  Error code high
  uint8_t reserved[8]; // +0x05 .. +0x0c  not used
  uint8_t attr;        // +0x0d.b  Attribute / Seek mode
  void *addr;          // +0x0e.l  Buffer address
  uint32_t status;     // +0x12.l  Bytes / Buffer / Result status
  void *fcb;           // +0x16.l  FCB
} __attribute__((packed, aligned(2)));

struct dos_devheader {
  struct dos_devheader *next;  // +0x00.l  Link pointer
  uint16_t type;       // +0x04.w  Device type
  void *strategy;      // +0x06.l  Strategy routine entry point
  void *interrupt;     // +0x0a.l  Interrupt routine entry point
  char name[8];        // +0x0e .. +0x15  Device name
  struct bgenabler_data *data;
} __attribute__((packed, aligned(2)));

// Human68kスレッド管理構造体
typedef struct huthread {
    struct huthread *next;          // 次のスレッド管理構造体へのポインタ
    uint8_t waitflg;                // スレッド待機フラグ
    uint8_t count;                  // 減算カウンタ
    uint8_t inicount;               // カウンタ初期値
    uint8_t doscallno;              // 実行中のDOSコール番号
    void *psp;                      // スレッドのPSPへのポインタ
    uint32_t usp;                   // スレッドのレジスタ退避領域
    uint32_t dreg[8];
    uint32_t areg[7];
    uint16_t sr;
    uint32_t pc;
    uint32_t ssp;
    uint16_t indosflg;              // INDOSフラグ
    void *indosptr;                 // INDOSポインタ
    void *commbuf;                  // スレッド間通信バッファへのポインタ
    char name[16];                  // スレッド名
    uint32_t remain;                // 待機時間残量
    void *membegin;                 // プロセスのメモリブロック先頭アドレス
    void *memend;                   // プロセスのメモリブロック最終アドレス+1
} huthread_t;

// バックグラウンド機能パラメータ
struct bgenabler_param {
    uint8_t maxthreads;
    uint8_t threads;
    uint8_t level;
    uint8_t slice;
    uint8_t opt_c;
    uint8_t opt_d;
    uint8_t opt_x;
    uint8_t opt_n;
};

// バックグラウンド機能データ (実体はhead.Sに配置)
struct bgenabler_data {
    int magic;                      // "BGE\x01"

    // 常駐部のデータ、関数エントリ
    huthread_t *thread_table;       // スレッド管理構造体
    void *timerc_intr;              // Timer-C割り込み処理
    void *nmi_intr;                 // INTERRUPTスイッチ割り込み処理
    void *trap12_intr;              // COPYキー割り込み処理
    void *trap12_intr_nop;          // COPYキー割り込み処理 (無効化する場合)
    void *dos_change_pr;            // Human68kのスレッド切り替え時に呼び出される

    // バックグラウンド機能パラメータ設定値
    struct bgenabler_param param;

    void *memblock;                 // 常駐部のメモリブロック
    void *old_nmivect;              // NMI割り込みベクタの新旧設定値
    void *new_nmivect;
    void *old_trap12vect;           // trap #12割り込みベクタの新旧設定値
    void *new_trap12vect;
    void *old_timerc;               // Timer-C割り込みベクタの新旧設定値
    void *new_timerc;
    void *old_iocs_timerdst;        // IOCS _TIMERDSTの新旧設定値
    void *new_iocs_timerdst;
    void *old_dos_change_pr;        // DOS _CHANGE_PRの新旧設定値
    void *new_dos_change_pr;
};

#define BGENABLER_MAGIC 0x42474501    // "BGE\x01"

// Human68kワークエリア

#define MEMEND          (*(void **)0x1c00)          // 現プロセスのメモリ最終アドレス+1
#define MEMBEGIN        (*(void **)0x1c04)          // 現プロセスのメモリ先頭アドレス
#define THRSWREQ        (*(uint8_t *)0x1c14)        // スレッド切り替え要求フラグ
#define THREADTBL       (*(huthread_t **)0x1c50)    // スレッド管理構造体テーブルへのポインタ
#define CURRENT         (*(huthread_t **)0x1c54)    // 現在実行中のスレッド管理構造体へのポインタ
#define MAXTHREADS      (*(uint16_t *)0x1c58)       // 最大スレッド数 - 1
#define NUMTHREADS      (*(uint16_t *)0x1c5a)       // 現在のスレッド数

//****************************************************************************
// Global variables
//****************************************************************************

extern struct dos_devheader devheader;          // Human68kのデバイスヘッダ
extern huthread_t thread_table[32];             // スレッド管理構造体
extern struct bgenabler_data bgenabler_data;    // バックグラウンド機能データ

static bool opt_r;

//****************************************************************************
// Private functions
//****************************************************************************

// 割り込み禁止状態にする
static inline uint16_t save_irq(void)
{
    uint16_t oldirq;
    __asm__ volatile (
        "move.w %%sr,%0\n"
        "ori.w  #0x0700,%%sr\n"
        : "=d"(oldirq) : : "memory"
    );
    return oldirq;
}

// 割り込み状態を元に戻す
static inline void restore_irq(uint16_t sr)
{
    __asm__ volatile (
        "move.w %0,%%sr\n"
        : : "d"(sr) : "memory"
    );
}

// 次のデバイスが name であるデバイスヘッダを探す
static struct dos_devheader *find_devheader(char *name)
{
    // Human68kからNULデバイスドライバを探す
    char *p = *(char **)0x001c20;   // 先頭のメモリブロック
    while (memcmp(p, "NUL     ", 8) != 0) {
        p += 2;
    }

    // デバイスドライバのリンクをたどって name の前のデバイスヘッダを探す
    // (name == NULLなら最後のデバイスヘッダを返す)
    struct dos_devheader *devh = (struct dos_devheader *)(p - 14);
    while (devh->next != (struct dos_devheader *)-1) {
        if (name && memcmp(devh->next->name, name, 8) == 0) {
            return devh;
        }
        devh = devh->next;
    }
    return name ? NULL : devh;
}

// 10進数を出力
static void putdec(int n)
{
    char buf[12];
    char *p = buf + sizeof(buf) - 1;
    *--p = '\0';
    do {
        *--p = '0' + (n % 10);
        n /= 10;
    } while (n > 0);
    _dos_print(p);
}

//****************************************************************************
// BG Task Enabler/Disabler
//****************************************************************************

// バックグラウンド機能を有効化する
static int bgenable(struct bgenabler_data *data, struct bgenabler_param *param)
{
    int stat = save_irq();

    if (CURRENT != NULL) {
        restore_irq(stat);
        _dos_print("既にバックグラウンドタスクが有効になっています\r\n");
        return -1;
    }

    // スレッド管理構造体の初期化
    huthread_t *p = data->thread_table;
    for (int i = 0; i < param->threads; i++) {
        memset(p, 0, sizeof(huthread_t));
        p->next = (i == param->threads - 1) ? data->thread_table : p + 1;
        p->waitflg = 0xff;
        p->count = 0xff;
        p->inicount = 0xff;
        p->doscallno = 0xff;
        p++;
    }

    // Human68kメインスレッドを初期化
    p = data->thread_table;
    p->waitflg = 0;
    p->count = 0;
    p->inicount = param->level - 1;
    p->psp = *(void **)0x013d0a;        // Human68k内部ワーク(v3.02)
    p->commbuf = (void *)0x00e792;      // Human68k内部ワーク(v3.02)
    p->membegin = MEMBEGIN;
    p->memend = MEMEND;
    strcpy(p->name, "Human68k system");
    p->remain = 0;

    // タイムスライス用タイマの設定
    if (param->opt_c) {
        // Timer-Cを使用する (50us * 200 = 10ms周期)
        data->new_timerc = data->timerc_intr;
        data->old_timerc = _dos_intvcs(0x45, data->new_timerc);
    }
    if (param->opt_d) {
        // Timer-Dを使用する (50us * 20 = 1ms周期)
        if (_iocs_timerdst((void *)0x00e018, 7, 20) != 0) {
            restore_irq(stat);
            _dos_print("Timer-Dが使用中です\r\n");
            return -1;
        }
        data->new_iocs_timerdst = (void *)0x00e762;
        data->old_iocs_timerdst = _dos_intvcs(0x016b, data->new_iocs_timerdst);
    }

    // タイムスライスを初期化
    *(uint8_t *)0x00e790 = param->slice;    // Human68k内部ワーク(v3.02)
    *(uint8_t *)0x00e791 = param->slice;    // Human68k内部ワーク(v3.02)

    // DOS _CHANGE_PRの設定
    data->new_dos_change_pr = data->dos_change_pr;
    data->old_dos_change_pr = _dos_intvcs(0xffff, data->new_dos_change_pr);

    // NMI割り込みベクタの設定
    data->new_nmivect = data->nmi_intr;
    data->old_nmivect = _dos_intvcs(0x1f, data->new_nmivect);

    // trap #12割り込みベクタの設定 (COPYキー処理)
    if (!param->opt_n) {
        data->new_trap12vect = param->opt_x ? data->trap12_intr_nop : data->trap12_intr;
        data->old_trap12vect = _dos_intvcs(0x2c, data->new_trap12vect);
    }

    // スレッド関連ワークエリアを設定
    MAXTHREADS = param->threads - 1;
    NUMTHREADS = 0;
    THREADTBL = data->thread_table;
    CURRENT = data->thread_table;

    // 現在の設定値を記録
    data->param = *param;

    restore_irq(stat);
    return 0;
}

// バックグラウンド機能を無効化する
static int bgdisable(struct bgenabler_data *data)
{
    int stat = save_irq();

    if (CURRENT == NULL) {
        restore_irq(stat);
        return 0;           // 既にBGタスク無効になっている場合は何もしない
    }

    // 動作中のバックグラウンドタスクが存在しないかチェック
    if (NUMTHREADS > 0) {
        restore_irq(stat);
        _dos_print("動作中のバックグラウンドタスクが存在するため状態変更できません\r\n");
        return -1;
    }

    // 割り込みベクタが変更されていないかチェック
    if ((*(void **)(0x1f * 4) != data->new_nmivect) ||
        (data->old_timerc != NULL && *(void **)(0x45 * 4) != data->new_timerc) ||
        (data->old_iocs_timerdst != NULL && *(void **)(0x016b * 4) != data->new_iocs_timerdst) ||
        (data->old_trap12vect != NULL && *(void **)(0x2c * 4) != data->new_trap12vect) ||
        (*(void **)(0x1800 + 0xff * 4) != data->new_dos_change_pr)) {
        restore_irq(stat);
        _dos_print("割り込みベクタが変更されているため状態変更できません\r\n");
        return -1;
    }

    // DOS _CHANGE_PRの割り込みベクタを元に戻す
    _dos_intvcs(0xffff, data->old_dos_change_pr);
    data->old_dos_change_pr = NULL;

    // NMI割り込みベクタを元に戻す
    _dos_intvcs(0x1f, data->old_nmivect);
    data->old_nmivect = NULL;

    // trap #12割り込みベクタを元に戻す
    if (data->old_trap12vect != NULL) {
        _dos_intvcs(0x2c, data->old_trap12vect);
        data->new_trap12vect = data->old_trap12vect;
        data->old_trap12vect = NULL;
    }

    // タイムスライス用タイマを元に戻す
    if (data->old_timerc != NULL) {
        // Timer-Cを使用していた場合はTimer-Cの割り込みベクタを元に戻す
        _dos_intvcs(0x45, data->old_timerc);
        data->old_timerc = NULL;
    }
    if (data->old_iocs_timerdst != NULL) {
        // Timer-Dを使用していた場合はIOCS _TIMERDST処理アドレスを元に戻して割り込み停止
        _dos_intvcs(0x016b, data->old_iocs_timerdst);
        data->old_iocs_timerdst = NULL;
        _iocs_timerdst(NULL, 0, 0);
    }

    //スレッド関連ワークエリアをクリア
    MAXTHREADS = 0;
    NUMTHREADS = 0;
    THREADTBL = NULL;
    CURRENT = NULL;
    THRSWREQ = 0;       // スレッド切り替え要求フラグをクリア

    restore_irq(stat);
    return 0;
}

//****************************************************************************
// Command line parsing and help
//****************************************************************************

static int parse_cmdline(const char *p, bool issys, struct bgenabler_param *param)
{
    _dos_print("X680x0 Background Task Enabler version " GIT_REPO_VERSION "\r\n");

    if ((_dos_vernum() & 0xffff) != 0x0302) {
        _dos_print("Human68kのバージョンがv3.02でないため実行できません\r\n");
        return -2;
    }

    // Human68k v3.02にパッチを当てる
    void fpu030patch(void);
    fpu030patch();

    if (issys) {
        while (*p++ != '\0')    // デバイスドライバ名をスキップする
            ;
    } else {
        p++;                    // 文字数をスキップする
    }

    int num_params = 0;
    int v;
    int res = 0;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '/' || *p == '-') {
            res = 1;
            p++;
            switch (tolower(*p++)) {
            case 'r':
                opt_r = true;
                break;
            case 'c':
                param->opt_c = true;
                param->opt_d = false;
                break;
            case 'd':
                param->opt_d = true;
                param->opt_c = false;
                break;
            case 'x':
                param->opt_x = true;
                param->opt_n = false;
                break;
            case 'n':
                param->opt_n = true;
                break;
            default:
                return -1;
            }
        } else if (*p == '+') {
            res = 1;
            p++;
            switch (tolower(*p++)) {
            case 'x':
                param->opt_x = false;
                break;
            case 'n':
                param->opt_n = false;
                break;
            default:
                return -1;
            }
        } else {
            res = 1;
            v = atoi(p);
            switch (num_params++) {
            case 0:
                if (v < 2 || v > 32) {
                    return -1;
                }
                if (v > param->maxthreads) {
                    _dos_print("最大スレッド数は常駐時の上限より大きくできません\r\n");
                    return -2;
                }
                param->threads = v;
                break;
            case 1:
                if (v < 2 || v > 255) {
                    return -1;
                }
                param->level = v;
                break;
            case 2:
                if (v < 1 || v > 100) {
                    return -1;
                }
                param->slice = v;
                break;
            default:
                return -1;
            }
            while (*p != ' ' && *p != '\t' && *p != '\0') {
                p++;
            }
        }

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (issys) {
           p += strlen(p) + 1;
        }
    }

    return res;
}

static void help(void)
{
    _dos_print(
        "使用法: bgenabler                                                  - 現在の状態を表示\r\n"
        "        bgenabler [オプション][<スレッド数>][<レベル>][<スライス>] - 常駐/設定変更\r\n"
        "        bgenabler -r                                               - 常駐解除\r\n"
        "オプション:\r\n"
        "  -r                 常駐解除\r\n"
        "  -c                 BGタスク処理にTimer-Cを使用する (タイムスライスは10ms単位)\r\n"
        "  -d                 BGタスク処理にTimer-Dを使用する (タイムスライスは1ms単位)\r\n"
        "  -x / +x            COPYキーを無視する / しない\r\n"
        "  -n / +n            COPYキー処理ベクタ(trap #12)を変更しない / 変更する\r\n"
        "  <スレッド数>       BGタスクの最大スレッド数 (2-32) (default:32)\r\n"
        "  <レベル>           メインスレッドの優先度レベル (2-255) (default:10)\r\n"
        "  <スライス>         タイムスライスの長さ(ms) (1-100) (default:10)\r\n"
    );
    _dos_exit2(1);
}

//****************************************************************************
// Program entry
//****************************************************************************

// CONFIG.SYSでの登録処理
int interrupt(void)
{
    extern struct dos_reqheader *reqheader;

    // Initialize以外はエラー
    if (reqheader->command != 0x00) {
        return 0x700d;
    }

    _dos_print("\r\n");

    // パラメータを解析する
    struct bgenabler_param param = bgenabler_data.param;
    int res = parse_cmdline((const char *)reqheader->status, true, &param);
    if (res < 0) {
        if (res == -1) {
            _dos_print("パラメータが不正です\r\n");
        }
        return 0x700d;
    }

    if (param.opt_c || param.opt_d) {
        if (bgenable(&bgenabler_data, &param) < 0) {
            return 0x700d;
        }
        _dos_print("Timer-");
        _dos_print(param.opt_c ? "C" : "D");
        _dos_print("を用いてバックグラウンドタスク機能を有効にしました\r\n");
    } else {
        _dos_print("バックグラウンドタスク機能を無効にしました\r\n");
    }

    bgenabler_data.param.maxthreads = param.threads;
    reqheader->addr = &thread_table[param.threads];
    return 0;
}

void _start(void)
{
    const char *cmdl;
    __asm__ volatile ("move.l %%a2,%0" : "=r"(cmdl)); // コマンドラインへのポインタ

    _dos_super(0);
    struct dos_devheader *devh = find_devheader("BGENB*/-");
    struct bgenabler_data *data = devh ? devh->next->data : &bgenabler_data;
    struct bgenabler_param param = data->param;

    int res = parse_cmdline(cmdl, false, &param);
    if (res < 0) {
        if (res == -1) {
            help();
        }
        _dos_exit2(1);
    }

    if (res == 0) {
        // コマンドラインオプションがない場合は現在の状態を表示
        _dos_print("常駐状態              : ");
        if (devh != NULL) {
            _dos_print((data->memblock != NULL) ? "常駐" : "CONFIG.SYSで登録");
        } else {
            _dos_print("非常駐");
        }
        _dos_print("\r\nバックグラウンドタスク: ");
        if (CURRENT == NULL) {
            _dos_print("無効");
        } else {
            _dos_print("有効");
            _dos_print("\r\nBGタスク処理          : Timer-");
            _dos_print((data->old_timerc != NULL) ? "C" : "D");
            _dos_print("\r\n最大スレッド数        : ");
            putdec(MAXTHREADS + 1);
            _dos_print("\r\nメインスレッド優先度  : ");
            struct huthread *main_thread = THREADTBL;
            putdec(main_thread->inicount + 1);
            _dos_print("\r\nタイムスライス        : ");
            int slice = *(uint8_t *)0x00e791;
            if (data->old_timerc != NULL) {
                slice = (slice + 9) / 10 * 10;    // Timer-Cは10ms単位なので切り上げて表示
            }
            putdec(slice);
            _dos_print("ms");
        }
        if (devh != NULL) {
            _dos_print("\r\nCOPYキー処理          : ");
            if (data->old_trap12vect != NULL) {
                _dos_print((data->new_trap12vect == data->trap12_intr_nop) ? "無効" : "有効");
            } else {
                _dos_print("変更なし");
            }
        }

        _dos_print("\r\n");

    } else if (devh != NULL) {
        // 既に常駐済みの場合
        if (data->magic != BGENABLER_MAGIC) {
            _dos_print("常駐している bgenabler のバージョンが異なります\r\n");
            _dos_exit2(1);
        }

        // 一旦常駐解除する
        if (bgdisable(data) < 0) {
            _dos_exit2(1);
        }

        if (!opt_r) {
            // -r オプションがなければ設定変更

            if (bgenable(data, &param) < 0) {
                _dos_exit2(1);
            }
            _dos_print("bgenabler の設定を変更しました\r\n");

        } else {
            // -r オプションがあれば常駐解除

            if (data->memblock != NULL) {
                devh->next = devh->next->next;
                _dos_print("bgenabler の常駐を解除しました\r\n");
                _dos_mfree(data->memblock);
            } else {
                _dos_print("bgenabler を無効化しました\r\n");
            }
        }

        // 常駐終了でない場合、プロセス終了時にtrap #12ベクタが親プロセスの保持している値に
        // 戻されてしまうので、親に遡ってPSP内のベクタも変更する
        int stat = save_irq();
        struct dos_mep *mep = &((struct dos_mep *)_dos_getpdb())[-1];   // 自身のメモリ管理ポインタ
        while (mep) {
            struct dos_psp *psp = (struct dos_psp *)&mep[1];
            psp->trap12 = data->new_trap12vect;
            mep = mep->parent_mp;
        }
        restore_irq(stat);

    } else {
        // 常駐していない場合
        if (opt_r) {
            _dos_print("常駐していません\r\n");
            _dos_exit();
        }
        if (!param.opt_c && !param.opt_d) {
            _dos_print("Timer-CかTimer-Dのいずれかを指定してください\r\n");
            _dos_exit2(1);
        }

        if (bgenable(data, &param) < 0) {
            _dos_exit2(1);
        }

        // 常駐終了
        data->memblock = _dos_getpdb();
        devh = find_devheader(NULL);
        devh->next = &devheader;

        _dos_print("bgenabler が常駐しました\r\n");
        bgenabler_data.param.maxthreads = param.threads;
        _dos_keeppr((int)&thread_table[param.threads] - (int)&devheader, 0);
    }

    _dos_exit();
}
