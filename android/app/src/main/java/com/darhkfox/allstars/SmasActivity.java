package com.darhkfox.allstars;

import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import org.libsdl.app.SDLActivity;

/**
 * The port's activity: SDL runs the games (libmain.so); this adds the ROM picker and the UI font rasteriser.
 * Android has no folder next to the program to drop the ROM in, so the player picks it with the system file
 * picker and it is copied into app storage (files/roms), where the ROM search looks.
 */
public class SmasActivity extends SDLActivity {
    private static final int PICK_ROMS = 0x534D;
    private static final int PICK_IMAGE = 0x534E;
    // the launcher's pictures (a card's cover or the background): the name it's saved as, and the result
    // (-1 while the picker is open / nothing yet, 0 nothing copied, 1 copied)
    private static volatile String imageName = "";
    private static volatile int imageResult = -1;
    private static final long MAX_FILE = 16L << 20;
    // set when the picker returns: the number of files copied (-1 while the picker is open / nothing yet)
    private static volatile int pickResult = -1;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    // ---- ROM picker (called from native code, app_android.cpp) ----------------------------------------
    // A web page (the update check: GitHub's latest release), its body; null if it couldn't be had.
    // Called from the native update checker's own thread.
    public static String httpGet(String url) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(8000);
            c.setReadTimeout(8000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "SuperMarioAllStars-port");
            if (c.getResponseCode() != 200) return null;
            InputStream in = c.getInputStream();
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while ((n = in.read(b)) > 0 && o.size() < (1 << 20)) o.write(b, 0, n);
            in.close();
            return o.toString("UTF-8");
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    public static void pickRoms() {
        pickResult = -1;
        final SDLActivity act = mSingleton;
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                try {
                    act.startActivityForResult(i, PICK_ROMS);
                } catch (Exception e) {
                    pickResult = 0;
                }
            }
        });
    }

    // ---- the launcher's pictures (called from native code, app_android.cpp) -------------------------
    // The player's own cover or background picture, copied to files/covers/<name>.png (read by its content:
    // a JPEG under that name is fine).
    public static void pickImage(String name) {
        imageName = name;
        imageResult = -1;
        final SDLActivity act = mSingleton;
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("image/*");
                try {
                    act.startActivityForResult(i, PICK_IMAGE);
                } catch (Exception e) {
                    imageResult = 0;
                }
            }
        });
    }

    public static int pollImageResult() {
        int r = imageResult;
        if (r >= 0) imageResult = -1;
        return r;
    }

    private boolean copyImage(Uri uri, File out) {
        File tmp = new File(out.getParentFile(), out.getName() + ".tmp");
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream os = new FileOutputStream(tmp)) {
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                total += n;
                if (total > (48L << 20)) throw new Exception("too big a picture");
            }
        } catch (Exception e) {
            tmp.delete();
            return false;
        }
        out.delete();
        return tmp.renameTo(out);
    }

    // polled from native code: -1 while the picker is open, else the number of files copied (once)
    public static int pollPickResult() {
        int r = pickResult;
        if (r >= 0) pickResult = -1;
        return r;
    }

    private String displayName(Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception e) {
            name = null;
        }
        if (name == null || name.isEmpty()) name = "rom_" + System.currentTimeMillis() + ".sfc";
        return name.replace('/', '_').replace('\\', '_');
    }

    private boolean copyIn(Uri uri, File dir) {
        File out = new File(dir, displayName(uri));
        File tmp = new File(dir, out.getName() + ".tmp");
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream os = new FileOutputStream(tmp)) {
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                total += n;
                if (total > MAX_FILE) throw new Exception("too big for an SNES ROM");
            }
        } catch (Exception e) {
            tmp.delete();
            return false;
        }
        out.delete();
        return tmp.renameTo(out);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == PICK_IMAGE) {
            int ok = 0;
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                File dir = new File(getFilesDir(), "covers");
                dir.mkdirs();
                String name = imageName.replace('/', '_').replace('\\', '_');
                for (String ext : new String[] { ".jpg", ".jpeg" }) new File(dir, name + ext).delete();
                if (copyImage(data.getData(), new File(dir, name + ".png"))) ok = 1;
            }
            imageResult = ok;
            return;
        }
        if (requestCode != PICK_ROMS) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        int copied = 0;
        if (resultCode == RESULT_OK && data != null) {
            File dir = new File(getFilesDir(), "roms");
            dir.mkdirs();
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null) for (int k = 0; k < clip.getItemCount(); k++) uris.add(clip.getItemAt(k).getUri());
            else if (data.getData() != null) uris.add(data.getData());
            for (Uri u : uris) if (copyIn(u, dir)) copied++;
        }
        pickResult = copied;
    }

    // ---- UI font glyphs (called from native code, video_gles.cpp) ------------------------------------
    // Renders each code point with the system font at `px` pixels. meta (filled in): [0] ascent, [1] line
    // height, then per code point: advance x 64, left, top (relative to the pen and baseline), width, height;
    // width 0 = nothing to draw, advance -1 = no glyph. Returns the 8-bit coverage bitmaps, one after another.
    // `title`: the title font (Luckiest Guy, in the app's assets; the bold system font if it can't be read).
    private static Typeface titleFace;
    public static byte[] renderGlyphs(int[] cps, float px, boolean bold, boolean title, int[] meta) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        Typeface tf = Typeface.create(Typeface.DEFAULT, bold ? Typeface.BOLD : Typeface.NORMAL);
        if (bold && Build.VERSION.SDK_INT >= 28) tf = Typeface.create(Typeface.DEFAULT, 600, false);
        if (title) {
            if (titleFace == null) {
                try {
                    titleFace = Typeface.createFromAsset(mSingleton.getAssets(), "LuckiestGuy-Regular.ttf");
                } catch (Exception e) {
                    titleFace = Typeface.DEFAULT_BOLD;
                }
            }
            tf = titleFace;
        }
        p.setTypeface(tf);
        p.setTextSize(px);
        p.setColor(0xFFFFFFFF);
        Paint.FontMetrics fm = p.getFontMetrics();
        meta[0] = (int) Math.ceil(-fm.ascent);
        meta[1] = (int) Math.ceil(fm.descent - fm.ascent + fm.leading);
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        Rect r = new Rect();
        for (int i = 0; i < cps.length; i++) {
            String s = new String(Character.toChars(cps[i]));
            int m = 2 + i * 5;
            if (cps[i] != ' ' && Build.VERSION.SDK_INT >= 23 && !p.hasGlyph(s)) {
                meta[m] = -1;
                continue;
            }
            meta[m] = Math.round(p.measureText(s) * 64);
            p.getTextBounds(s, 0, s.length(), r);
            if (r.width() <= 0 || r.height() <= 0) {
                meta[m + 3] = meta[m + 4] = 0;
                continue;
            }
            int w = r.width() + 2, h = r.height() + 2;   // a pixel of room for anti-aliasing on each side
            Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
            new Canvas(b).drawText(s, 1 - r.left, 1 - r.top, p);
            ByteBuffer buf = ByteBuffer.allocate(b.getRowBytes() * h);
            b.copyPixelsToBuffer(buf);
            byte[] all = buf.array();
            for (int y = 0; y < h; y++) bytes.write(all, y * b.getRowBytes(), w);
            b.recycle();
            meta[m + 1] = r.left - 1;
            meta[m + 2] = r.top - 1;
            meta[m + 3] = w;
            meta[m + 4] = h;
        }
        return bytes.toByteArray();
    }
}
