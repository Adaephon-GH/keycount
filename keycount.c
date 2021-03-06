/************************************************************************
 * keycount.c
 *
 * Copyright 2012 Albin Olsson
 * Copyright 2014 William Hatch
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***********************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XTest.h>
#include <X11/XKBlib.h>

#include <glib.h>


/************************************************************************
 * Internal data types
 ***********************************************************************/


typedef struct _XCape_t
{
    Display *data_conn;
    Display *ctrl_conn;
    XRecordContext record_ctx;
    pthread_t sigwait_thread;
    sigset_t sigset;
    Bool debug;
} XCape_t;

typedef struct _Entry
{
  int timesPressed;
  GHashTable *subtable;
} Entry;

/************************************************************************
 * Internal function declarations
 ***********************************************************************/
void *sig_handler (void *user_data);

void intercept (XPointer user_data, XRecordInterceptData *data);

void printTable(FILE* stream, GHashTable *tab, int depth);
void flushTableToFile();
void freeEntry(Entry *entry);

GHashTable *symtab = NULL;
Bool useDigraphs = True;
Bool useTrigraphs = True;
Bool useOnlyLevel1 = False;
Bool ignoreDuplicatesP = False;
Bool ignoreDuplicates2P = False;
#define NAMESIZE 50
char symName[NAMESIZE];
#define LASTKEYSIZE 3
int lastKeys[LASTKEYSIZE];
Bool extraDebug = False;
int nreceived = 0;
#define DUMP_THRESHOLD 1000
int dumpThreshold = DUMP_THRESHOLD;

FILE* outfile;




/************************************************************************
 * Main function
 ***********************************************************************/
int main (int argc, char **argv)
{
    XCape_t *self = malloc (sizeof (XCape_t));
    int dummy, ch;
    outfile = NULL;
    self->debug = False;
    char filename[FILENAME_MAX];
    int gotFileNameP = 0;

    symtab = g_hash_table_new_full(g_int_hash, g_int_equal, 
        (GDestroyNotify)free, (GDestroyNotify)freeEntry);

    while ((ch = getopt (argc, argv, "i2dlf:c:")) != -1)
    {
        switch (ch)
        {
        case 'i':
            ignoreDuplicatesP = True;
            break;
        case '2':
            ignoreDuplicates2P = True;
            break;
        case 'l':
            useOnlyLevel1 = True;
            break;
        case 'd':
            self->debug = True;
            break;
        case 'f':
            strncpy(filename, optarg, FILENAME_MAX);
            gotFileNameP = 1;
            break;
        case 'c':
            dumpThreshold = atoi(optarg);
            break;
        default:
            fprintf (stdout, "Usage: %s [-f <outputfile>]\n", argv[0]);
            fprintf (stdout, "    [-c <dump threshold>]\n");
            fprintf (stdout, "    [-dil]\n");
            fprintf (stdout, "-l to use only level 1 symbols\n");
            fprintf (stdout, "   (useful for getting stats on physical keys\n");
            fprintf (stdout, "-i ignore duplicates (2+ consecutive strokes\n");
            fprintf (stdout, "   on same key) - useful to ignore large numbers\n");
            fprintf (stdout, "   of presses due to eg. holding a key down.\n");
            fprintf (stdout, "-2 ignore duplicates after 2\n");
            fprintf (stdout, "   (better than -i)\n");
            fprintf (stdout, "-d debug -- don't detach as daemon\n");
            fprintf (stdout, "   Dumps output to outputfile (defaults to\n");
            fprintf (stdout, "   keycount.log) every time it reads the\n");
            fprintf (stdout, "   threshold number of keys.\n");
            return EXIT_SUCCESS;
        }
    }
    if (!gotFileNameP) {
        strncpy(filename, "keycount.log", FILENAME_MAX);
    }

    outfile = fopen(filename, "a");

    self->data_conn = XOpenDisplay (NULL);
    self->ctrl_conn = XOpenDisplay (NULL);

    if (!self->data_conn || !self->ctrl_conn)
    {
        fprintf (stderr, "Unable to connect to X11 display. Is $DISPLAY set?\n");
        exit (EXIT_FAILURE);
    }
    if (!XQueryExtension (self->ctrl_conn,
                "XTEST", &dummy, &dummy, &dummy))
    {
        fprintf (stderr, "Xtst extension missing\n");
        exit (EXIT_FAILURE);
    }
    if (!XRecordQueryVersion (self->ctrl_conn, &dummy, &dummy))
    {
        fprintf (stderr, "Failed to obtain xrecord version\n");
        exit (EXIT_FAILURE);
    }
    if (!XkbQueryExtension (self->ctrl_conn, &dummy, &dummy,
            &dummy, &dummy, &dummy))
    {
        fprintf (stderr, "Failed to obtain xkb version\n");
        exit (EXIT_FAILURE);
    }



    if (self->debug != True)
        daemon (0, 0);

    sigemptyset (&self->sigset);
    sigaddset (&self->sigset, SIGINT);
    sigaddset (&self->sigset, SIGTERM);
    sigaddset (&self->sigset, SIGHUP);
    pthread_sigmask (SIG_BLOCK, &self->sigset, NULL);

    pthread_create (&self->sigwait_thread,
            NULL, sig_handler, self);

    XRecordRange *rec_range = XRecordAllocRange();
    rec_range->device_events.first = KeyPress;
    rec_range->device_events.last = ButtonRelease;
    XRecordClientSpec client_spec = XRecordAllClients;

    self->record_ctx = XRecordCreateContext (self->ctrl_conn,
            0, &client_spec, 1, &rec_range, 1);

    if (self->record_ctx == 0)
    {
        fprintf (stderr, "Failed to create xrecord context\n");
        exit (EXIT_FAILURE);
    }

    XSync (self->ctrl_conn, False);

    if (!XRecordEnableContext (self->data_conn,
                self->record_ctx, intercept, (XPointer)self))
    {
        fprintf (stderr, "Failed to enable xrecord context\n");
        exit (EXIT_FAILURE);
    }

    if (!XRecordFreeContext (self->ctrl_conn, self->record_ctx))
    {
        fprintf (stderr, "Failed to free xrecord context\n");
    }

    XCloseDisplay (self->ctrl_conn);
    XCloseDisplay (self->data_conn);

    if (self->debug) fprintf (stdout, "main exiting\n");

    return EXIT_SUCCESS;
}


/************************************************************************
 * Internal functions
 ***********************************************************************/
void *sig_handler (void *user_data)
{
    XCape_t *self = (XCape_t*)user_data;
    int sig;

    if (self->debug) fprintf (stdout, "sig_handler running...\n");

    sigwait(&self->sigset, &sig);

    if (self->debug) fprintf (stdout, "Caught signal %d!\n", sig);
    flushTableToFile();

    if (!XRecordDisableContext (self->ctrl_conn,
                self->record_ctx))
    {
        fprintf (stderr, "Failed to disable xrecord context\n");
        exit(EXIT_FAILURE);
    }

    XSync (self->ctrl_conn, False);

    if (self->debug) fprintf (stdout, "sig_handler exiting...\n");

    return NULL;
}


void rotateLastkeys (int newsym)
{
    int i;
    for(i = LASTKEYSIZE-1; i > 0; --i) {
        lastKeys[i] = lastKeys[i-1];
    }
    lastKeys[0] = newsym;
}

void incKeyInTable (GHashTable *table, int keysym)
{
    Entry *entry = g_hash_table_lookup(table, &keysym);
    if (NULL == entry)
    {
        int *key = malloc(sizeof(int));
        *key = keysym;
        entry = malloc(sizeof(Entry));
        entry->timesPressed = 1;
        entry->subtable = NULL;
        g_hash_table_insert(table, key, entry);
    }
    else
    {
        entry->timesPressed += 1;
    }
}

typedef struct _PrintData {
    FILE* stream;
    int depth;
} PrintData;

void printEntry(gpointer *key, gpointer *value, gpointer *data)
{
    PrintData *d = (PrintData*) data;
    int symbol = *(int*) key;
    Entry *entry = (Entry*) value;

    if (NULL == entry || NULL == data)
        return;
    char* symstring = XKeysymToString(symbol);
    int i;
    for (i = 0; i < d->depth; ++i) {
        fprintf(d->stream, "*   ");
    }
    fprintf(d->stream, "%s:%d\n", symstring, entry->timesPressed);
    printTable(d->stream, entry->subtable, d->depth+1);
}
  
void printTable(FILE* stream, GHashTable *tab, int depth)
{
    if (NULL == tab)
        return;
    PrintData *d = malloc(sizeof(PrintData));
    d->stream = stream;
    d->depth = depth;
    g_hash_table_foreach(tab, (GHFunc)printEntry, d);
    free(d);
}

void freeEntry(Entry *entry) {
    if (NULL == entry)
        return;
    g_hash_table_destroy(entry->subtable);
    free(entry);
}

void flushTableToFile(){
    printTable(outfile, symtab, 0);
    fprintf(outfile, "##########################################\n");
    fflush(outfile);
    g_hash_table_destroy(symtab);
    symtab = g_hash_table_new_full(g_int_hash, g_int_equal, 
        (GDestroyNotify)free, (GDestroyNotify)freeEntry);
    nreceived = 0;
}

void intercept (XPointer user_data, XRecordInterceptData *data)
{
    XCape_t *self = (XCape_t*)user_data;

    if (data->category == XRecordFromServer)
    {
        int     key_event = data->data[0];
        KeyCode key_code  = data->data[1];
        static int mods = 0;
        //int extrabytes_garbage;

        int xksym = XkbKeycodeToKeysym (self->ctrl_conn, key_code, 0, 0);
        int modsForKey = XkbKeysymToModifiers(self->ctrl_conn, xksym);
        mods ^= modsForKey;

        // this doesn't seem to be working for me
        //XkbTranslateKeySym(self->ctrl_conn, &xksym, mods, symName, NAMESIZE, &extrabytes_garbage);
        // so I'll do the translations myself -- assuming a fairly normal layout
        int shiftMods = XkbKeysymToModifiers(self->ctrl_conn, XK_Shift_L);
        int capsMods = XkbKeysymToModifiers(self->ctrl_conn, XK_Caps_Lock);
        int l3Mods = XkbKeysymToModifiers(self->ctrl_conn, XK_ISO_Level3_Shift);
        int level = 0;
        if (mods & (shiftMods|capsMods)) {
            level += 1; // close enough.  I won't bother cancelling shift and caps, because my layout doesn't
        }
        if (mods & l3Mods) {
            level += 2; // because level3 shift adds two... of course!
        }

        int transKey = XkbKeycodeToKeysym (self->ctrl_conn, key_code, 0, level);


        int sym = xksym;
        if (!useOnlyLevel1) {
            sym = modsForKey ? xksym : transKey; // assume that modifiers are one-level
        }

        //XkbKeycodeToKeysym (display, keycode, group, level)
        //XKeysymToString(xksym)
        if (key_event == KeyPress) {
            if ((ignoreDuplicatesP && sym == lastKeys[0]) ||
                (ignoreDuplicates2P && sym == lastKeys[0] && sym == lastKeys[1])) {
                goto end;
            }
            ++nreceived;
            rotateLastkeys(sym);
            // record new key
            incKeyInTable(symtab, sym);
            
            Entry *oldEntry, *oldOldEntry, *subEntry;
            if (useDigraphs && nreceived > 1) {
                oldEntry = g_hash_table_lookup(symtab, lastKeys+1);
                if(NULL == oldEntry->subtable) {
                    oldEntry->subtable = g_hash_table_new_full(g_int_hash, g_int_equal, 
                        (GDestroyNotify)free, (GDestroyNotify)freeEntry);
                }
                incKeyInTable(oldEntry->subtable, sym);
                
                if (useTrigraphs && nreceived > 2) {
                    oldOldEntry = g_hash_table_lookup(symtab, lastKeys+2);
                    subEntry = g_hash_table_lookup(oldOldEntry->subtable, lastKeys+1);
                    if(NULL == subEntry->subtable) {
                        subEntry->subtable = g_hash_table_new_full(g_int_hash, g_int_equal, 
                            (GDestroyNotify)free, (GDestroyNotify)freeEntry);
                    }
                    incKeyInTable(subEntry->subtable, sym);
                }
            }

            if (nreceived == dumpThreshold) {
                flushTableToFile();
            }
            
        }

    }

end:

    XRecordFreeData (data);
}

