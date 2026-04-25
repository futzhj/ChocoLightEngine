package com.chocolight.engine;

import org.libsdl.app.SDLActivity;

/**
 * ChocoLight 引擎 Android Activity — 继承 SDLActivity
 * SDL3 负责 GL Surface 创建、事件循环、输入处理
 * 原生层 main() 通过 SDL_main 调用
 */
public class ChocoLightActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL3",         // SDL3 共享库 (必须先加载)
            "chocolight"    // 引擎 + Lumen + main 入口
        };
    }
}
