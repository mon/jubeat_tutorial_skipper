#pragma once
#include <stdint.h>
#include <windows.h>

extern "C" {
    void GFAVSSwapBuffers(HDC hdc, float framerate, int swap_interval);
    bool GFFSMObjectIsShutting(void *fsm);
    void GFFSMObjectSetNextState(void *fsm, uint32_t state);
    void GFFSMObjectSetNextData(void *fsm, void *data);
    void GFFSMLogPutLeaveState(void *fsm, uint32_t state, uint32_t *request, bool leave);
    void GFFSMObjectSetTransiting(void *fsm, bool transiting);
    uint32_t GFInputDDevGetStateMap(int a, int b);
    void GFFSMLogPutOnMessage(void *fsm, uint32_t state, uint32_t *request);
    void GFDrawScreen(int x, int y, int w, int h);
    void GFDrawReadyContext(void);
    void GFDrawReadyArrays(void);
    void GFDrawSetCapability(uint32_t cap, uint32_t mask, int enable);
    void GFDrawAlphaFunc(uint32_t func, uint32_t ref);
    void GFDrawBlendFunc(uint32_t sfactor, uint32_t dfactor);
    void GFDrawBegin(uint32_t mode);
    void GFDrawEnd(void);
    void GFDrawColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void GFDrawVertex2f(float x, float y);
    void GFDrawTexCoord2f(float u, float v);
}
