/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package org.blender.blender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.IBinder;

/**
 * Keeps the process in the foreground while a render runs.
 *
 * A render can take longer than the screen timeout. Without a foreground service Android is free
 * to suspend the process once the screen goes off, and reclaim it soon after; the render then
 * stalls with nothing in the log to show why. The notification is also the only progress signal a
 * user gets when the render outlasts their attention.
 *
 * Started and stopped by BlenderActivity.setBusy(), which GHOST calls around RE_RenderFrame.
 */
public class BlenderRenderService extends Service {

  private static final String CHANNEL_ID = "blender-render";
  private static final int NOTIFICATION_ID = 1;

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    NotificationManager manager = (NotificationManager) getSystemService(
        Context.NOTIFICATION_SERVICE);
    NotificationChannel channel = new NotificationChannel(
        CHANNEL_ID, "Rendering", NotificationManager.IMPORTANCE_LOW);
    /* A rendering notice is not a message; it should not buzz, peek, or leave a badge. */
    channel.setShowBadge(false);
    channel.setSound(null, null);
    manager.createNotificationChannel(channel);

    Notification notification = new Notification.Builder(this, CHANNEL_ID)
        .setContentTitle("Blender is rendering")
        .setContentText("The screen stays on until the render finishes.")
        .setSmallIcon(android.R.drawable.stat_sys_upload)
        .setOngoing(true)
        .build();
    startForeground(NOTIFICATION_ID, notification);
    return START_NOT_STICKY;
  }

  @Override
  public IBinder onBind(Intent intent) {
    return null;
  }
}
