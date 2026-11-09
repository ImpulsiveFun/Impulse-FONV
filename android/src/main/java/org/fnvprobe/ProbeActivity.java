package org.fnvprobe;



import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;

import android.content.Intent;


import android.net.Uri;

import android.os.Bundle;
import android.os.SystemClock;

import android.util.Log;

import android.view.Gravity;

import android.view.Surface;

import android.view.SurfaceHolder;

import android.view.SurfaceView;

import android.view.View;

import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.view.KeyEvent;
import android.view.MotionEvent;

import android.widget.LinearLayout;

import android.widget.TextView;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;

import java.io.ByteArrayOutputStream;

import java.io.InputStream;

import java.io.File;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicBoolean;

import android.content.Context;

import java.security.MessageDigest;



public final class ProbeActivity extends Activity {

    private static final int PICK_EXE = 1, PICK_GAME_ZIP = 2, PICK_DLC_ZIP = 3, PICK_MOD_ZIP = 4;

    static synchronized native String runProbe();

    static native String expectedExecutableDigest();
    static native String virtualAdapterDescription();

    private static native void configureSurfaceGeometry(Surface surface, int width, int height);
    static void configureSurface(Surface surface) {
        configureSurfaceGeometry(surface, 0, 0);
    }

    static synchronized native String runSurfaceCheck();
    static native void inputKey(int code, boolean down);
    static native void requestGameFrame();
    static native void reportGamePointer();
    static native void requestTraceStop();
    private static native void setPlayClosing(boolean closing);
    static native void configureInputScript(boolean enabled);
    static native void inputMotion(int dx, int dy);
    static native boolean gameMenuActive();
    private static native long presentedFrames();
    static native boolean checkTouchAim(boolean fire, boolean forward, int dx);
    static native void inputWheel(int delta);
    static native void inputButton(int button, boolean down);
    static native void inputGamepad(boolean connected, int buttons, int leftTrigger, int rightTrigger,
                                    int leftX, int leftY, int rightX, int rightY);
    static synchronized native boolean runGamepadCheck(boolean connected, int expectedRightX);
    static synchronized native String runInputCheck(boolean verify);
    static synchronized native String runReleasedInputCheck();

    static synchronized native void configureWindow(int width, int height);
    private static synchronized native void configureRenderMode(int width, int height, int textureSkip);

    static synchronized native String runStartup(byte[] file, String digest, String gameRoot, int seconds);

    static String traceStartup(byte[] file,String digest,String gameRoot,int seconds) throws Exception {
        return traceStartup(file, digest, gameRoot, seconds, null, TextureDetail.HIGH);
    }

    private static String traceStartup(byte[] file, String digest, String gameRoot, int seconds,
                                       GameConfiguration.Resolution resolution, TextureDetail detail) throws Exception {
        java.util.concurrent.FutureTask<String> task=new java.util.concurrent.FutureTask<>(() -> {
            GameConfiguration.prepare(new File(gameRoot), virtualAdapterDescription(), resolution);
            configureRenderMode(resolution == null ? 0 : resolution.width, resolution == null ? 0 : resolution.height,
                                detail.mipSkip);
            try {
                return runStartup(file,digest,gameRoot,seconds);
            } finally {
                configureRenderMode(0, 0, 0);
            }
        });
        new Thread(null,task,"fnv-game",32L*1024*1024).start();
        return task.get();
    }




    static String prepareGameRoot(Context context) throws IOException {

        File root = new File(context.getFilesDir(), "game");

        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Could not create the game folder.");

        return root.getCanonicalPath();

    }

    private static final AtomicBoolean PLAYING = new AtomicBoolean();
    private static final int MAX_EXECUTABLE_BYTES = 32 * 1024 * 1024;
    private static final String RESOLUTION_SETTING = "render-resolution";
    private static final String FPS_SETTING = "show-fps";
    private static final String TEXTURE_DETAIL_SETTING = "texture-detail";
    private enum TextureDetail {
        HIGH("High", 0), MEDIUM("Medium", 1), LOW("Low", 2);
        final String label;
        final int mipSkip;
        TextureDetail(String label, int mipSkip) {
            this.label = label;
            this.mipSkip = mipSkip;
        }
    }
    private static final long FPS_SAMPLE_MS = 1000;
    private TextView result;
    private Button startup, assets, dlcAssets, mods, lookSettings, touchLayout, resolutionButton, exteriorDetailButton;
    private Button textureDetailButton;
    private TextureDetail textureDetail = TextureDetail.HIGH;
    private GameConfiguration.Resolution resolution;
    private RadioGroup controlMode;
    private int controllerModeId;
    private SurfaceView preview;
    private FrameLayout previewFrame;
    private ScrollView setupScroll;
    private InputOverlay controls;
    private CheckBox fpsSetting;
    private TextView fpsMeter;
    private long fpsSampleTime, fpsSampleFrames;
    private final Runnable sampleFps = this::updateFpsReading;
    private volatile boolean libraryReady;
    private boolean traceRunning, resumed, busy;
    private boolean normalSession;

    void setGameRunning(boolean running) {
        traceRunning = running;
        setRequestedOrientation(running ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        setupScroll.setVisibility(running ? View.GONE : View.VISIBLE);
        previewFrame.setVisibility(running ? View.VISIBLE : View.GONE);
        InputOverlay.setImmersive(this, running);
        if (running) preview.requestFocus();
        updateGameControls();
    }

    private void updateGameControls() {
        controls.setActive(traceRunning && resumed);
        preview.setKeepScreenOn(traceRunning && resumed);
        fpsMeter.removeCallbacks(sampleFps);
        boolean showFps = normalSession && traceRunning && resumed && fpsSetting.isChecked();
        fpsMeter.setVisibility(showFps ? View.VISIBLE : View.GONE);
        if (showFps) {
            fpsMeter.setText("FPS —");
            fpsSampleFrames = presentedFrames();
            fpsSampleTime = SystemClock.elapsedRealtime();
            fpsMeter.postDelayed(sampleFps, FPS_SAMPLE_MS);
        }
    }

    private void updateFpsReading() {
        long now = SystemClock.elapsedRealtime();
        long frames = presentedFrames();
        long elapsed = now - fpsSampleTime;
        if (elapsed > 0) {
            long fps = Math.round((frames - fpsSampleFrames) * 1000.0 / elapsed);
            fpsMeter.setText(fps + " FPS");
        }
        fpsSampleFrames = frames;
        fpsSampleTime = now;
        fpsMeter.postDelayed(sampleFps, FPS_SAMPLE_MS);
    }

    private void setBusy(boolean value) {
        busy = value;
        startup.setEnabled(!value && libraryReady);
        assets.setEnabled(!value && libraryReady);
        dlcAssets.setEnabled(!value && libraryReady);
        mods.setEnabled(!value && libraryReady);
        lookSettings.setEnabled(!value && libraryReady);
        touchLayout.setEnabled(!value);
        resolutionButton.setEnabled(!value && libraryReady);
        exteriorDetailButton.setEnabled(!value && libraryReady);
        textureDetailButton.setEnabled(!value && libraryReady);
        fpsSetting.setEnabled(!value);
        for (int i = 0; i < controlMode.getChildCount(); ++i) controlMode.getChildAt(i).setEnabled(!value);
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LauncherStyle style = new LauncherStyle(this);
        LinearLayout setupPanel = style.page();
        style.addBlock(setupPanel, style.hero());

        LinearLayout playCard = style.card(null);
        startup = style.button("Play", true);
        startup.setOnClickListener(view -> {
            File exe = installedExecutable();
            if (exe.isFile()) launch(exe, null);
            else pickFile(PICK_EXE, "*/*");
        });
        style.addAction(playCard, startup);
        String savedResolution = getPreferences(MODE_PRIVATE).getString(RESOLUTION_SETTING, "");
        for (GameConfiguration.Resolution mode : GameConfiguration.Resolution.values()) {
            if (mode.name().equals(savedResolution)) resolution = mode;
        }
        resolutionButton = style.button(resolutionLabel(), false);
        resolutionButton.setOnClickListener(view -> showResolutionPicker());
        style.addAction(playCard, resolutionButton);
        String savedDetail = getPreferences(MODE_PRIVATE).getString(TEXTURE_DETAIL_SETTING, "");
        for (TextureDetail detail : TextureDetail.values())
            if (detail.name().equals(savedDetail)) textureDetail = detail;
        textureDetailButton = style.button(textureDetailLabel(), false);
        textureDetailButton.setOnClickListener(view -> showTextureDetail());
        style.addAction(playCard, textureDetailButton);
        exteriorDetailButton = style.button("Exterior detail", false);
        exteriorDetailButton.setOnClickListener(view -> showExteriorDetail());
        style.addAction(playCard, exteriorDetailButton);
        fpsSetting = new CheckBox(this);
        fpsSetting.setText("Show FPS in game");
        fpsSetting.setTextColor(0xfff8ebd4);
        fpsSetting.setChecked(getPreferences(MODE_PRIVATE).getBoolean(FPS_SETTING, false));
        fpsSetting.setOnCheckedChangeListener((view, checked) ->
                getPreferences(MODE_PRIVATE).edit().putBoolean(FPS_SETTING, checked).apply());
        style.addAction(playCard, fpsSetting);
        result = style.status();
        result.setText("Checking game runtime…");
        playCard.addView(result, new LinearLayout.LayoutParams(-1, -2));
        style.addBlock(setupPanel, playCard);

        LinearLayout controlsCard = style.card("CONTROLS");
        controlMode = new RadioGroup(this);
        controlMode.setOrientation(RadioGroup.VERTICAL);
        controlMode.setContentDescription("Controls for this session");
        RadioButton touch = new RadioButton(this);
        touch.setId(View.generateViewId());
        touch.setText("Touch controls");
        style.styleRadio(touch);
        RadioButton controller = new RadioButton(this);
        controllerModeId = View.generateViewId();
        controller.setId(controllerModeId);
        controller.setText("Controller only");
        style.styleRadio(controller);
        controlMode.addView(touch);
        controlMode.addView(controller);
        controlsCard.addView(controlMode, new LinearLayout.LayoutParams(-1, -2));
        controlsCard.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            int horizontal = getResources().getConfiguration().fontScale > 1.25f ? style.dp(530) : style.dp(400);
            boolean wide = right - left >= horizontal;
            int orientation = wide ? RadioGroup.HORIZONTAL : RadioGroup.VERTICAL;
            if (controlMode.getOrientation() == orientation) return;
            controlMode.setOrientation(orientation);
            touch.setLayoutParams(new RadioGroup.LayoutParams(wide ? 0 : -1, -2, wide ? 1f : 0f));
            controller.setLayoutParams(new RadioGroup.LayoutParams(wide ? 0 : -1, -2, wide ? 1f : 0f));
        });

        lookSettings = style.button("Right-stick sensitivity", false);
        lookSettings.setOnClickListener(view -> controls.showLookSettings(controlMode.getCheckedRadioButtonId() != controllerModeId));
        style.addAction(controlsCard, lookSettings);
        touchLayout = style.button("Edit touch layout", false);
        touchLayout.setOnClickListener(view -> controls.showTouchLayout());
        style.addAction(controlsCard, touchLayout);
        controlMode.setOnCheckedChangeListener((group, checked) -> {
            boolean useController = checked == controllerModeId;
            lookSettings.setText(useController ? "Right-stick sensitivity" : "Touch aim sensitivity");
            touchLayout.setVisibility(useController ? View.GONE : View.VISIBLE);
            getPreferences(MODE_PRIVATE).edit().putBoolean("controller-only", useController).apply();
        });
        controlMode.check(getPreferences(MODE_PRIVATE).getBoolean("controller-only", false)
                ? controllerModeId : touch.getId());
        style.addBlock(setupPanel, controlsCard);

        LinearLayout contentCard = style.card("GAME CONTENT");
        mods = style.button("Mods", false);
        mods.setOnClickListener(view -> showMods());
        style.addAction(contentCard, mods);
        dlcAssets = style.button("Import DLC (ZIP)", false);
        dlcAssets.setOnClickListener(view -> pickFile(PICK_DLC_ZIP, "application/zip"));
        style.addAction(contentCard, dlcAssets);
        assets = style.button("Import game files (ZIP)", false);
        assets.setOnClickListener(view -> pickFile(PICK_GAME_ZIP, "application/zip"));
        style.addAction(contentCard, assets);
        style.addBlock(setupPanel, contentCard);

        previewFrame = new FrameLayout(this);
        previewFrame.setVisibility(View.GONE);
        preview = new SurfaceView(this);
        preview.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {}
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (libraryReady) configureSurfaceGeometry(holder.getSurface(), width, height);
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                if (libraryReady) configureSurface(null);
            }
        });
        previewFrame.addView(preview, new FrameLayout.LayoutParams(-1, -1));
        controls = new InputOverlay(this, preview);
        previewFrame.addView(controls, new FrameLayout.LayoutParams(-1, -1));
        fpsMeter = new TextView(this);
        fpsMeter.setTextColor(0xffffffff);
        fpsMeter.setBackgroundColor(0xc0000000);
        fpsMeter.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        fpsMeter.setTextSize(14);
        fpsMeter.setPadding(style.dp(8), style.dp(4), style.dp(8), style.dp(4));
        fpsMeter.setVisibility(View.GONE);
        FrameLayout.LayoutParams fpsPosition = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        fpsPosition.topMargin = style.dp(16);
        fpsPosition.setMarginEnd(style.dp(16));
        previewFrame.addView(fpsMeter, fpsPosition);

        FrameLayout root = new FrameLayout(this);
        setupScroll = new ScrollView(this);
        setupScroll.setBackgroundColor(LauncherStyle.BACKGROUND);
        setupScroll.setFillViewport(true);
        FrameLayout centered = new FrameLayout(this);
        centered.addView(setupPanel, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        setupScroll.addView(centered);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            setupScroll.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                        | android.view.WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return insets;
            });
        }
        root.addView(setupScroll, new FrameLayout.LayoutParams(-1, -1));
        root.addView(previewFrame, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        setBusy(true);

        new Thread(() -> {
            String message;
            boolean ready = false;
            try {
                System.loadLibrary("fnvprobe");
                configureWindow(getResources().getDisplayMetrics().widthPixels, getResources().getDisplayMetrics().heightPixels);
                String check = runProbe();
                Log.i("FNVProbe", check);
                ready = check.startsWith("PASS:");
                message = ready
                        ? installedExecutable().isFile()
                            ? "Select Continue in New Vegas to resume your save."
                            : "Import your game ZIP below, or tap Play to locate the executable."
                        : check;
            } catch (LinkageError error) {
                Log.e("FNVProbe", "Unable to load the game runtime", error);
                message = "Unable to load the game runtime: " + error.getMessage();
            }
            String outcome = message;
            boolean loaded = ready;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                libraryReady = loaded;
                result.setText(outcome);
                setBusy(false);
                if (loaded && state == null && resolution == null && !isFinishing()) showResolutionPicker();
            });
        }, "fnv-probe").start();
    }

    private String resolutionLabel() {
        return resolution == null ? "Render resolution" : "Render resolution · " + resolution.label();
    }

    private void showResolutionPicker() {
        ActivityManager manager = getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        android.util.DisplayMetrics display = getResources().getDisplayMetrics();
        GameConfiguration.Resolution suggested = GameConfiguration.Resolution.suggested(
                memory.totalMem, Math.min(display.widthPixels, display.heightPixels), manager.isLowRamDevice());
        GameConfiguration.Resolution[] modes = GameConfiguration.Resolution.values();
        String[] labels = new String[modes.length];
        for (int i = 0; i < modes.length; i++) {
            labels[i] = modes[i].label() + (modes[i] == suggested ? " (suggested)" : "");
        }
        new AlertDialog.Builder(this)
                .setTitle("Render resolution")
                .setSingleChoiceItems(labels, (resolution == null ? suggested : resolution).ordinal(), null)
                .setPositiveButton("Apply", (dialog, which) -> {
                    int selected = ((AlertDialog) dialog).getListView().getCheckedItemPosition();
                    if (selected < 0) return;
                    resolution = modes[selected];
                    getPreferences(MODE_PRIVATE).edit().putString(RESOLUTION_SETTING, resolution.name()).apply();
                    resolutionButton.setText(resolutionLabel());
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private String textureDetailLabel() {
        return "Texture detail · " + textureDetail.label;
    }

    private void showTextureDetail() {
        TextureDetail[] options = TextureDetail.values();
        String[] labels = {"High · full detail", "Medium · smaller textures", "Low · smallest textures"};
        new AlertDialog.Builder(this)
                .setTitle("Texture detail")
                .setSingleChoiceItems(labels, textureDetail.ordinal(), null)
                .setPositiveButton("Apply", (dialog, which) -> {
                    int selected = ((AlertDialog) dialog).getListView().getCheckedItemPosition();
                    if (selected < 0) return;
                    textureDetail = options[selected];
                    getPreferences(MODE_PRIVATE).edit().putString(TEXTURE_DETAIL_SETTING, textureDetail.name()).apply();
                    textureDetailButton.setText(textureDetailLabel());
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void showExteriorDetail() {
        final File root;
        final String[] values;
        try {
            root = new File(prepareGameRoot(this));
            values = GameConfiguration.exteriorDetail(root);
        } catch (IOException error) {
            result.setText("Could not read exterior detail settings: " + error.getMessage());
            return;
        }

        LinearLayout fields = new LinearLayout(this);
        fields.setOrientation(LinearLayout.VERTICAL);
        int padding = new LauncherStyle(this).dp(24);
        fields.setPadding(padding, 0, padding, 0);
        fields.setFocusableInTouchMode(true);
        TextView explanation = new TextView(this);
        explanation.setText("Lower values draw less grass and distant scenery. Applies the next time you play.");
        fields.addView(explanation);
        EditText grass = detailField(fields, "Grass draw distance (0–7000)", values[0]);
        EditText objects = detailField(fields, "Scenery draw distance multiplier (1–15)", values[1]);
        Button performance = new Button(this);
        performance.setText("Performance preset");
        performance.setOnClickListener(view -> {
            grass.setText(GameConfiguration.PERFORMANCE_GRASS);
            objects.setText(GameConfiguration.PERFORMANCE_OBJECTS);
        });
        fields.addView(performance);
        Button standard = new Button(this);
        standard.setText("Standard preset");
        standard.setOnClickListener(view -> {
            grass.setText(GameConfiguration.STANDARD_GRASS);
            objects.setText(GameConfiguration.STANDARD_OBJECTS);
        });
        fields.addView(standard);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(fields);
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Exterior detail")
                .setView(scroll)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Apply", null)
                .show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
            String grassValue = detailValue(grass, 0, 7000);
            String objectValue = detailValue(objects, 1, 15);
            if (grassValue == null || objectValue == null) return;
            try {
                GameConfiguration.saveExteriorDetail(root, grassValue, objectValue);
                dialog.dismiss();
                result.setText("Exterior detail saved. Tap Play to use the new settings.");
            } catch (IOException error) {
                grass.setError("Could not save settings: " + error.getMessage());
            }
        });
    }

    private EditText detailField(LinearLayout fields, String title, String value) {
        TextView label = new TextView(this);
        label.setText(title);
        EditText field = new EditText(this);
        field.setId(View.generateViewId());
        label.setLabelFor(field.getId());
        field.setSingleLine(true);
        field.setInputType(android.text.InputType.TYPE_CLASS_NUMBER | android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL);
        field.setText(value);
        fields.addView(label);
        fields.addView(field);
        return field;
    }

    private static String detailValue(EditText field, int min, int max) {
        String value = field.getText().toString().trim().replace(',', '.');
        try {
            if (value.matches("[0-9]{1,4}(\\.[0-9]{1,4})?")) {
                double number = Double.parseDouble(value);
                if (number >= min && number <= max) return value;
            }
        } catch (NumberFormatException ignored) {}
        field.setError("Enter a value from " + min + " to " + max + " with up to four decimal places.");
        return null;
    }

    private File installedExecutable() {
        File imported = new File(getFilesDir(), "game/FalloutNV.exe");
        return imported.isFile() ? imported : new File(getFilesDir(), "probe/FalloutNV.exe");
    }

    private void pickFile(int request, String mimeType) {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType(mimeType);
        startActivityForResult(picker, request);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null || busy) return;
        if (requestCode == PICK_GAME_ZIP || requestCode == PICK_DLC_ZIP)
            importGame(data.getData(), requestCode == PICK_DLC_ZIP);
        else if (requestCode == PICK_MOD_ZIP) importMod(data.getData());
        else if (requestCode == PICK_EXE) launch(null, data.getData());
    }

    private void launch(File executable, Uri selected) {
        if (busy || !libraryReady) return;
        if (!PLAYING.compareAndSet(false, true)) {
            result.setText("The previous game session is still closing.");
            return;
        }
        boolean controllerOnly = controlMode.getCheckedRadioButtonId() == controllerModeId;
        setBusy(true);
        result.setText("Verifying your game executable…");
        new Thread(() -> {
            try {
                byte[] bytes;
                try (InputStream stream = selected == null ? Files.newInputStream(executable.toPath())
                        : getContentResolver().openInputStream(selected)) {
                    if (stream == null) throw new IOException("Could not read the selected executable.");
                    ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                    byte[] chunk = new byte[64 * 1024];
                    for (int n; (n = stream.read(chunk)) != -1;) {
                        if (buffer.size() + n > MAX_EXECUTABLE_BYTES) throw new IOException("The executable is too large.");
                        buffer.write(chunk, 0, n);
                    }
                    bytes = buffer.toByteArray();
                }
                StringBuilder hex = new StringBuilder(64);
                for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes)) {
                    hex.append(Character.forDigit((value >> 4) & 15, 16));
                    hex.append(Character.forDigit(value & 15, 16));
                }
                String digest = hex.toString();
                if (!digest.equals(expectedExecutableDigest()))
                    throw new IOException("The executable differs from the one used to build this APK. Import the matching game ZIP.");
                String root = prepareGameRoot(this);
                if (selected != null) {
                    File temporary = File.createTempFile("exe-", ".tmp", new File(root));
                    try {
                        Files.write(temporary.toPath(), bytes);
                        Files.move(temporary.toPath(), new File(root, "FalloutNV.exe").toPath(),
                                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                    } finally { Files.deleteIfExists(temporary.toPath()); }
                }
                runOnUiThread(() -> beginGame(bytes, digest, root, controllerOnly));
            } catch (Exception | LinkageError error) {
                finishGame("Could not start the game: " + error.getMessage());
            }
        }, "fnv-verify").start();
    }

    private void beginGame(byte[] executable, String digest, String root, boolean controllerOnly) {
        if (isFinishing() || isDestroyed()) {
            PLAYING.set(false);
            return;
        }
        controls.setPlayMode(controllerOnly);
        normalSession = true;
        setPlayClosing(false);
        setGameRunning(true);
        GameConfiguration.Resolution selectedResolution = resolution;
        TextureDetail selectedDetail = textureDetail;
        new Thread(() -> {
            String outcome;
            try {
                outcome = traceStartup(executable, digest, root, 0, selectedResolution, selectedDetail);
                Log.i("FNVProbe", outcome);
                if (outcome.startsWith("Guest requested ExitProcess(0)") || outcome.startsWith("Startup stopped by request"))
                    outcome = "Game closed. Tap Play, then Continue to resume a saved game.";
            } catch (Exception | LinkageError error) {
                Log.e("FNVProbe", "Game session failed", error);
                outcome = "Game session failed: " + error.getMessage();
            }
            finishGame(outcome);
        }, "fnv-play").start();
    }

    private void finishGame(String message) {
        runOnUiThread(() -> {
            PLAYING.set(false);
            if (isDestroyed()) return;
            normalSession = false;
            setGameRunning(false);
            result.setText(message);
            setBusy(false);
        });
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        return controls != null && controls.keyboard(event) || super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return controls != null && controls.gamepadMotion(event) || super.dispatchGenericMotionEvent(event);
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (controls != null && !focused) controls.releaseAll();
        if (focused && traceRunning) {
            InputOverlay.setImmersive(this, true);
            controls.refreshGamepad();
        }
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        updateGameControls();
    }

    @Override protected void onPause() {
        resumed = false;
        updateGameControls();
        super.onPause();
    }

    @Override protected void onDestroy() {
        fpsMeter.removeCallbacks(sampleFps);
        if (normalSession) setPlayClosing(true);
        else if (libraryReady && traceRunning) requestTraceStop();
        super.onDestroy();
    }

    private void showMods() {
        try {
            new ModMenu(this, new File(prepareGameRoot(this)),
                    () -> pickFile(PICK_MOD_ZIP, "application/zip")).show();
        } catch (IOException error) {
            result.setText("Could not open Mods: " + error.getMessage());
        }
    }

    private void importMod(Uri uri) {
        setBusy(true);
        result.setText("Reading your local mod ZIP…");
        new Thread(() -> {
            String message;
            boolean success = false;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("Could not open the selected archive.");
                String label = GameMods.importZip(in, new File(prepareGameRoot(this)),
                        text -> runOnUiThread(() -> { if (!isDestroyed()) result.setText(text); }));
                message = "Imported " + label + ". Enable it in Mods when ready.";
                success = true;
            } catch (Exception error) {
                message = "Mod import failed: " + error.getMessage();
            }
            String outcome = message;
            boolean imported = success;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                result.setText(outcome);
                setBusy(false);
                if (imported) showMods();
            });
        }, "fnv-mod-import").start();
    }

    private void importGame(Uri uri, boolean dlc) {
        setBusy(true);
        result.setText(dlc ? "Reading your local add-on archive…" : "Reading your local game archive…");
        new Thread(() -> {
            String message;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("Could not open the selected archive.");
                GameAssetImport.Progress progress =
                        text -> runOnUiThread(() -> { if (!isDestroyed()) result.setText(text); });
                message = dlc ? GameAssetImport.importDlcZip(in, getFilesDir(), expectedExecutableDigest(), progress)
                        : GameAssetImport.importZip(in, getFilesDir(), expectedExecutableDigest(), progress);
            } catch (Exception error) {
                message = "Import failed: " + error.getMessage();
            }
            String outcome = message;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                result.setText(outcome);
                setBusy(false);
            });
        }, "fnv-asset-import").start();
    }
}
