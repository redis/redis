#include <stdlib.h>

#include <hiredis.h>
#include <async.h>
#include <adapters/glib.h>

static GMainLoop *mainloop;

static void
connect_cb (const redisAsyncContext *ac G_GNUC_UNUSED,
            int status)
{
    if (status != REDIS_OK) {
        g_printerr("Failed to connect: %s\n", ac->errstr);
        g_main_loop_quit(mainloop);
    } else {
        g_printerr("Connected...\n");
    }
}

static void
disconnect_cb (const redisAsyncContext *ac G_GNUC_UNUSED,
               int status)
{
    if (status != REDIS_OK) {
        g_error("Failed to disconnect: %s", ac->errstr);
    } else {
        g_printerr("Disconnected...\n");
        g_main_loop_quit(mainloop);
    }
}

static void
command_cb(redisAsyncContext *ac,
           gpointer r,
           gpointer user_data G_GNUC_UNUSED)
{
    redisReply *reply = r;

    if (reply) {
        g_print("REPLY: %s\n", reply->str);
    }

    redisAsyncDisconnect(ac);
}

gint
main (gint argc     G_GNUC_UNUSED,
      gchar *argv[] G_GNUC_UNUSED)
{
    redisAsyncContext *ac;
    GMainContext *context = NULL;
    GSource *source;

    ac = redisAsyncConnect("127.0.0.1", 6379);
    if (ac->err) {
        g_printerr("%s\n", ac->errstr);
        exit(EXIT_FAILURE);
    }

    source = redis_source_new(ac);
    mainloop = g_main_loop_new(context, FALSE);
    g_source_attach(source, context);

    redisAsyncSetConnectCallback(ac, connect_cb);
    redisAsyncSetDisconnectCallback(ac, disconnect_cb);
    redisAsyncCommand(ac, command_cb, NULL, "SET key 1234");
    redisAsyncCommand(ac, command_cb, NULL, "GET key");

    g_main_loop_run(mainloop);

    return EXIT_SUCCESS;
}
