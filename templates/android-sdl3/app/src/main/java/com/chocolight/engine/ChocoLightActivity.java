package com.chocolight.engine;

import org.libsdl.app.SDLActivity;
import android.os.Bundle;

/**
 * ChocoLight 引擎 Android Activity — 继承 SDLActivity
 * SDL3 负责 GL Surface 创建、事件循环、输入处理
 * 原生层 main() 通过 SDL_main 调用
 */
public class ChocoLightActivity extends SDLActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 等待模拟器 GPU 驱动初始化完成, 避免 EGL 竞态
        try { Thread.sleep(300); } catch (InterruptedException ignored) {}
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL3",         // SDL3 共享库 (必须先加载)
            "chocolight"    // 引擎 + Lumen + main 入口
        };
    }
}
