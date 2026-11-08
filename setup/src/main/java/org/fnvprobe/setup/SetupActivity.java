package org.fnvprobe.setup;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;

public final class SetupActivity extends Activity {
    private static final int PICK_EXE = 1;
    private static final int EXE_LIMIT = 32 * 1024 * 1024;
    private static final String TESTED_EXE =
            "19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f";
    private TextView result;
    private Button select;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int padding = Math.round(24 * getResources().getDisplayMetrics().density);
        ScrollView scroll = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setPadding(padding, padding, padding, padding);
        content.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(content);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            scroll.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return insets;
            });
        }
        addText(content, "New Vegas Recomp Setup", 28, padding);
        addText(content, "Check this phone and your own PC game executable before building the playable app on your computer.", 16, padding);
        ActivityManager manager = getSystemService(ActivityManager.class);
        boolean arm64 = Arrays.asList(Build.SUPPORTED_ABIS).contains("arm64-v8a");
        boolean gles31 = manager != null && manager.getDeviceConfigurationInfo().reqGlEsVersion >= 0x00030001;
        addText(content, "ARM64  " + (arm64 ? "Supported" : "Required") + "\nOpenGL ES 3.1  "
                + (gles31 ? "Reported by device" : "Required"), 16, padding);

        select = new Button(this);
        select.setText("Check local FalloutNV.exe");
        select.setOnClickListener(view -> {
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            picker.setType("*/*");
            picker.addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(picker, PICK_EXE);
        });
        content.addView(select);
        result = addText(content, "Choose an executable to compare it with the tested build.", 15, padding);
        result.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        addText(content, "Next, follow BUILD.md in the repository to generate and install your personal APK. Import your game ZIP in the playable app.", 16, padding);
        setContentView(scroll);
    }

    private TextView addText(LinearLayout content, String text, int size, int bottom) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setPadding(0, 0, 0, bottom);
        content.addView(view);
        return view;
    }

    @Override protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request != PICK_EXE || code != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        select.setEnabled(false);
        result.setText("Checking the selected file…");
        new Thread(() -> {
            String message;
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("Could not open the file.");
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                byte[] buffer = new byte[64 * 1024];
                int total = 0;
                for (int count; (count = input.read(buffer)) != -1;) {
                    total += count;
                    if (total > EXE_LIMIT) throw new IOException("The selected file is too large.");
                    digest.update(buffer, 0, count);
                }
                StringBuilder hash = new StringBuilder(64);
                for (byte value : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", value & 255));
                message = TESTED_EXE.contentEquals(hash) ? "This file matches the executable used to test v150."
                        : "This file differs from the tested executable. Check BUILD.md for compatibility details.";
            } catch (Exception error) {
                message = "File check failed. " + error.getMessage();
            }
            String outcome = message;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                result.setText(outcome);
                select.setEnabled(true);
            });
        }, "setup-file-check").start();
    }
}
