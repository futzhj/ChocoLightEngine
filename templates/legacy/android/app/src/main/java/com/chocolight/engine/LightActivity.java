package com.chocolight.engine;

import android.app.Activity;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * ChocoLight Engine — Android 模板 Activity
 * 加载 libchocolight.so, 从 assets 读取 main.lua 并执行
 * Lua 的 print() 输出显示在屏幕上
 */
public class LightActivity extends Activity {
    static {
        System.loadLibrary("chocolight");
    }

    // JNI 方法：在后台线程执行 Lua 脚本
    private static native int nativeRunScript(
        AssetManager assetManager, String scriptName, OutputCallback callback);

    // Lua print() 回调接口
    public interface OutputCallback {
        void onOutput(String text);
    }

    private TextView outputView;
    private final StringBuilder outputBuffer = new StringBuilder();
    private Handler mainHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mainHandler = new Handler(Looper.getMainLooper());

        // 创建深色主题 UI
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#1a1a2e"));
        root.setPadding(32, 48, 32, 32);

        // 标题
        TextView title = new TextView(this);
        title.setText("🍫 ChocoLight Engine");
        title.setTextColor(Color.parseColor("#e94560"));
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, 24);
        root.addView(title);

        // 输出区域（可滚动）
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);

        outputView = new TextView(this);
        outputView.setTextColor(Color.parseColor("#00ff41"));
        outputView.setBackgroundColor(Color.parseColor("#0f3460"));
        outputView.setTextSize(14);
        outputView.setTypeface(Typeface.MONOSPACE);
        outputView.setPadding(16, 16, 16, 16);
        outputView.setMovementMethod(new ScrollingMovementMethod());
        outputView.setText("Initializing Lua VM...\n");

        scroll.addView(outputView);
        root.addView(scroll, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.MATCH_PARENT));

        setContentView(root);

        // 后台线程执行 Lua 脚本
        new Thread(() -> {
            appendOutput("Loading main.lua from assets...\n");
            int status = nativeRunScript(getAssets(), "main.lua", text ->
                mainHandler.post(() -> appendOutput(text + "\n"))
            );
            appendOutput("\n--- Script finished (status=" + status + ") ---\n");
        }).start();
    }

    private void appendOutput(String text) {
        mainHandler.post(() -> {
            outputBuffer.append(text);
            outputView.setText(outputBuffer.toString());
        });
    }
}
