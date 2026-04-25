package com.chocolight.engine;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/**
 * ChocoLight Engine — Android 模板 Activity
 * 加载 liblua51.so 并执行打包的 Lua 脚本
 */
public class LightActivity extends Activity {
    static {
        System.loadLibrary("lua51");
    }

    // JNI: 执行 assets 目录下的 Lua 脚本
    private static native int nativeRun(String assetPath);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setText("ChocoLight Engine Loading...");
        setContentView(tv);
        // 实际项目中这里会启动 OpenGL ES surface + Lua VM
    }
}
