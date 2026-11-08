package org.fnvprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.util.Collections;
import java.util.List;

final class ModMenu {
    private final Activity activity;
    private final File root;
    private final Runnable importZip;
    private final List<GameMods.Pack> packs;
    private final LinearLayout rows;
    private final TextView error;

    ModMenu(Activity activity, File root, Runnable importZip) throws IOException {
        this.activity = activity;
        this.root = root;
        this.importZip = importZip;
        packs = GameMods.load(root);
        rows = new LinearLayout(activity);
        rows.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        rows.setPadding(padding, 0, padding, padding);
        error = new TextView(activity);
    }

    void show() {
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(rows);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("Mods").setView(scroll)
                .setNegativeButton("Cancel", null).setNeutralButton("Import ZIP", null)
                .setPositiveButton("Done", null).create();
        render();
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> save(dialog, false));
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> save(dialog, true));
    }

    private void save(AlertDialog dialog, boolean importing) {
        try {
            if (!packs.isEmpty()) GameMods.save(root, packs);
        } catch (IOException failure) {
            error.setText(failure.getMessage());
            return;
        }
        dialog.dismiss();
        if (importing) importZip.run();
    }

    private void render() {
        rows.removeAllViews();
        TextView instructions = new TextView(activity);
        instructions.setText(packs.isEmpty() ? "No mods installed. Import a ZIP containing an ESP or ESM."
                : "Check mods to enable them. Mods at the bottom load later within each plugin type. Changes take effect on Play.");
        rows.addView(instructions);
        rows.addView(error);
        for (int i = 0; i < packs.size(); i++) {
            int index = i;
            GameMods.Pack pack = packs.get(i);
            LinearLayout row = new LinearLayout(activity);
            CheckBox enabled = new CheckBox(activity);
            enabled.setText(pack.label());
            enabled.setChecked(pack.enabled);
            enabled.setOnCheckedChangeListener((view, checked) -> pack.enabled = checked);
            row.addView(enabled, new LinearLayout.LayoutParams(0, -2, 1));
            addMoveButton(row, index, -1, "Up");
            addMoveButton(row, index, 1, "Down");
            rows.addView(row);
        }
    }

    private void addMoveButton(LinearLayout row, int index, int offset, String label) {
        Button move = new Button(activity);
        move.setText(label);
        move.setContentDescription("Move " + packs.get(index).label() + " " + label.toLowerCase(java.util.Locale.ROOT));
        int destination = index + offset;
        move.setEnabled(destination >= 0 && destination < packs.size());
        move.setOnClickListener(view -> {
            Collections.swap(packs, index, destination);
            render();
        });
        row.addView(move);
    }
}
