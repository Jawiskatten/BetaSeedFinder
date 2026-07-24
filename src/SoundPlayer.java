import javax.sound.sampled.AudioInputStream;
import javax.sound.sampled.AudioSystem;
import javax.sound.sampled.Clip;
import javax.sound.sampled.FloatControl;
import java.io.File;

public class SoundPlayer {
    private static final float VOLUME_DB = -14.0f; // lower = quieter. Try -18.0f if needed.

    private final Object lock = new Object();

    private Clip currentClip = null;
    private Thread currentThread = null;

    // True while best/mega is loading OR playing.
    private boolean protectedActive = false;

    private long version = 0;

    public void play(String path) {
        playInternal(path, false);
    }

    public void playProtected(String path) {
        playInternal(path, true);
    }

    public boolean playFirstExisting(String[] paths) {
        return playFirstExistingInternal(paths, false);
    }

    public boolean playProtectedFirstExisting(String[] paths) {
        return playFirstExistingInternal(paths, true);
    }

    private boolean playFirstExistingInternal(String[] paths, boolean protectedSound) {
        if (paths == null) {
            return false;
        }

        for (String path : paths) {
            if (path == null || path.isBlank()) {
                continue;
            }

            if (new File(path).exists()) {
                playInternal(path, protectedSound);
                return true;
            }
        }

        return false;
    }

    private void playInternal(String path, boolean protectedSound) {
        if (path == null || path.isBlank()) {
            return;
        }

        final long myVersion;

        synchronized (lock) {
            // If best/mega is loading or playing, ignore every new sound.
            if (protectedActive) {
                return;
            }

            version++;
            myVersion = version;

            stopCurrentLocked();

            protectedActive = protectedSound;
        }

        Thread thread = new Thread(() -> {
            Clip clip = null;

            try {
                File file = new File(path);

                if (!file.exists()) {
                    System.out.println("Sound file missing: " + file.getAbsolutePath());
                    return;
                }

                try (AudioInputStream audioStream = AudioSystem.getAudioInputStream(file)) {
                    clip = AudioSystem.getClip();
                    clip.open(audioStream);

                    if (clip.isControlSupported(FloatControl.Type.MASTER_GAIN)) {
                        FloatControl gain = (FloatControl) clip.getControl(FloatControl.Type.MASTER_GAIN);

                        float safeVolume = Math.max(
                                gain.getMinimum(),
                                Math.min(gain.getMaximum(), VOLUME_DB)
                        );

                        gain.setValue(safeVolume);
                    }

                    synchronized (lock) {
                        if (myVersion != version) {
                            closeClip(clip);
                            return;
                        }

                        currentClip = clip;
                        currentThread = Thread.currentThread();
                    }

                    clip.setFramePosition(0);
                    clip.start();

                    long soundLengthMs = Math.max(250, clip.getMicrosecondLength() / 1000);
                    long endTime = System.currentTimeMillis() + soundLengthMs + 150;

                    while (System.currentTimeMillis() < endTime) {
                        synchronized (lock) {
                            if (myVersion != version) {
                                break;
                            }
                        }

                        Thread.sleep(10);
                    }
                }
            } catch (InterruptedException ignored) {
                // Replaced by another non-protected sound.
            } catch (Exception e) {
                System.out.println("Sound failed: " + path);
                e.printStackTrace();
            } finally {
                if (clip != null) {
                    closeClip(clip);
                }

                synchronized (lock) {
                    // Only clear state if this is still the active sound.
                    if (myVersion == version) {
                        currentClip = null;
                        currentThread = null;
                        protectedActive = false;
                    }
                }
            }
        }, "SoundPlayer");

        thread.setDaemon(true);

        synchronized (lock) {
            currentThread = thread;
        }

        thread.start();
    }

    private void stopCurrentLocked() {
        if (currentClip != null) {
            closeClip(currentClip);
            currentClip = null;
        }

        if (currentThread != null) {
            currentThread.interrupt();
            currentThread = null;
        }

        protectedActive = false;
    }

    private void closeClip(Clip clip) {
        try {
            if (clip.isRunning()) {
                clip.stop();
            }

            clip.close();
        } catch (Exception ignored) {
        }
    }
}