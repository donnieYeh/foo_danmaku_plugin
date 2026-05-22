#ifndef DANMAKU_UI_H
#define DANMAKU_UI_H

#include <foobar2000/SDK/foobar2000.h>
#include <pfc/pfc.h>
#include <windows.h>

class DanmakuEngine;

class DanmakuUI : public ui_element {
public:
    DanmakuUI();
    static const GUID& g_get_guid();
    void get_name(pfc::string_base& p_out) override;
    GUID get_guid() override { return g_get_guid(); }
    GUID get_subclass() override { return ui_element_subclass_utility; }
    ui_element_config::ptr get_default_configuration() override;
    ui_element_instance_ptr instantiate(fb2k::hwnd_t p_parent, ui_element_config::ptr cfg, ui_element_instance_callback_ptr p_callback) override;
    ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr cfg) override { (void)cfg; return nullptr; }
    bool is_user_addable() override { return true; }

private:
    bool m_visible = true;
};

extern const GUID g_danmaku_guid;

class DanmakuUIWindow {
public:
    DanmakuUIWindow(HWND parent);
    ~DanmakuUIWindow();

    HWND getHwnd() const { return m_hwnd; }
    void setVisible(bool visible);
    void setEngine(DanmakuEngine* engine);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static const wchar_t* kClassName;

    HWND m_hwnd;
    HWND m_parent;
    bool m_visible;
    DanmakuEngine* m_engine;

    void onCreate();
    void onDestroy();
    void onShow();
    void onHide();
    void onSize(int w, int h);
    void onPaint();
    void onTimer();

    void startTimer();
    void stopTimer();
};

class DanmakuUIInstance : public ui_element_instance {
public:
    DanmakuUIInstance(HWND parent, ui_element_instance_callback_ptr callback);
    ~DanmakuUIInstance();

    fb2k::hwnd_t get_wnd() override { return m_wnd->getHwnd(); }
    void set_configuration(ui_element_config::ptr data) override {}
    ui_element_config::ptr get_configuration() override { return ui_element_config::g_create_empty(g_danmaku_guid); }
    GUID get_guid() override { return g_danmaku_guid; }
    GUID get_subclass() override { return ui_element_subclass_utility; }
    double get_focus_priority() override { return 0; }
    void set_default_focus() override {}

private:
    ui_element_instance_callback_ptr m_callback;
    DanmakuUIWindow* m_wnd;
};

#endif // DANMAKU_UI_H
