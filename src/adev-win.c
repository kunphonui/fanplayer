// 包含头文件
#include "adev.h"

#pragma warning(disable:4311)
#pragma warning(disable:4312)

// 内部常量定义
#define DEF_ADEV_BUF_NUM  3
#define DEF_ADEV_BUF_LEN  2048

// 内部类型定义
typedef struct
{
    ADEV_COMMON_MEMBERS

    HWAVEOUT hWaveOut;
    WAVEHDR *pWaveHdr;
    HANDLE   bufsem;
} ADEV_CONTEXT;

// 内部函数实现
static void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)dwInstance;
    switch (uMsg)
    {
    case WOM_DONE:
        c->bufcur = (int16_t*)c->pWaveHdr[c->head].lpData;
        c->cmnvars->apts = c->ppts[c->head];
        av_log(NULL, AV_LOG_INFO, "apts: %lld\n", c->cmnvars->apts);
        if (++c->head == c->bufnum) c->head = 0;
        ReleaseSemaphore(c->bufsem, 1, NULL);
        break;
    }
}

// 接口函数实现
void* adev_create(int type, int bufnum, int buflen, CMNVARS *cmnvars)
{
    ADEV_CONTEXT *ctxt = NULL;
    WAVEFORMATEX  wfx  = {0};
    BYTE         *pwavbuf;
    MMRESULT      result;
    int           i;

    // allocate adev context
    ctxt = (ADEV_CONTEXT*)calloc(1, sizeof(ADEV_CONTEXT));
    if (!ctxt) {
        av_log(NULL, AV_LOG_ERROR, "failed to allocate adev context !\n");
        exit(0);
    }

    bufnum         = bufnum ? bufnum : DEF_ADEV_BUF_NUM;
    buflen         = buflen ? buflen : DEF_ADEV_BUF_LEN;
    ctxt->bufnum   = bufnum;
    ctxt->buflen   = buflen;
    ctxt->ppts     = (int64_t*)calloc(bufnum, sizeof(int64_t));
    ctxt->pWaveHdr = (WAVEHDR*)calloc(bufnum, (sizeof(WAVEHDR) + buflen));
    ctxt->bufsem   = CreateSemaphore(NULL, bufnum, bufnum, NULL);
    ctxt->cmnvars  = cmnvars;
    if (!ctxt->ppts || !ctxt->pWaveHdr || !ctxt->bufsem) {
        av_log(NULL, AV_LOG_ERROR, "failed to allocate waveout buffer and waveout semaphore !\n");
        exit(0);
    }

    // init for audio
    wfx.cbSize          = sizeof(wfx);
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.wBitsPerSample  = 16;    // 16bit
    wfx.nSamplesPerSec  = ADEV_SAMPLE_RATE;
    wfx.nChannels       = 2;     // stereo
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    result = waveOutOpen(&ctxt->hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)waveOutProc, (DWORD_PTR)ctxt, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        CloseHandle(ctxt->bufsem);
        free(ctxt->ppts    );
        free(ctxt->pWaveHdr);
        free(ctxt);
        return NULL;
    }

    // init wavebuf
    pwavbuf = (BYTE*)(ctxt->pWaveHdr + bufnum);
    for (i=0; i<bufnum; i++) {
        ctxt->pWaveHdr[i].lpData         = (LPSTR)(pwavbuf + i * buflen);
        ctxt->pWaveHdr[i].dwBufferLength = buflen;
        waveOutPrepareHeader(ctxt->hWaveOut, &ctxt->pWaveHdr[i], sizeof(WAVEHDR));
    }

    // init software volume scaler
    ctxt->vol_zerodb = swvol_scaler_init(ctxt->vol_scaler, SW_VOLUME_MINDB, SW_VOLUME_MAXDB);
    ctxt->vol_curvol = ctxt->vol_zerodb;
    return ctxt;
}

void adev_destroy(void *ctxt)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    int           i;
    if (!ctxt) return;

    // before close wavout, we need reset it first,
    // otherwise it will cause crash on vs2013
    waveOutReset(c->hWaveOut);

    // unprepare wave header & close waveout device
    for (i=0; i<c->bufnum; i++) {
        if (c->hWaveOut) {
            waveOutUnprepareHeader(c->hWaveOut, &c->pWaveHdr[i], sizeof(WAVEHDR));
        }
    }
    waveOutClose(c->hWaveOut);

    // close semaphore
    CloseHandle(c->bufsem);

    // free memory
    free(c->ppts    );
    free(c->pWaveHdr);
    free(c);
}

void adev_lock(void *ctxt, AUDIOBUF **ppab)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    if (!ctxt) return;
    WaitForSingleObject(c->bufsem, -1);
    *ppab = (AUDIOBUF*)&c->pWaveHdr[c->tail];
}

void adev_unlock(void *ctxt, int64_t pts)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    int           multiplier, n;
    int16_t      *buf;
    if (!ctxt) return;

    //++ software volume scale
    multiplier = c->vol_scaler[c->vol_curvol];
    buf        = (int16_t*)c->pWaveHdr[c->tail].lpData;
    n          = c->pWaveHdr[c->tail].dwBufferLength / sizeof(int16_t);
    swvol_scaler_run(buf, n, multiplier);
    //-- software volume scale

    c->ppts[c->tail] = pts;
    waveOutWrite(c->hWaveOut, &c->pWaveHdr[c->tail], sizeof(WAVEHDR));
    if (++c->tail == c->bufnum) c->tail = 0;
}

void adev_pause(void *ctxt, int pause)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    if (!ctxt) return;
    if (pause) {
        waveOutPause(c->hWaveOut);
    } else {
        waveOutRestart(c->hWaveOut);
    }
}

void adev_reset(void *ctxt)
{
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    if (!ctxt) return;
    waveOutReset(c->hWaveOut);
    c->head = c->tail = 0;
    ReleaseSemaphore(c->bufsem, c->bufnum, NULL);
}

