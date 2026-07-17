#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "coro.h"
#include "game.h"
#include "tui.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

#define CTRL_P 16
#define CTRL_Q 17
#define KEY_Q 113
#define KEY_W 119

static bool read_attr, end_attr;
struct task_obj {
    struct xo_table tlb;
    enum tui_tab tab;
};

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static void listen_keyboard_handler(enum tui_tab *tab)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    if (attr_fd < 0)
        return;
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case CTRL_P:
            read(attr_fd, buf, 6);
            buf[0] = buf[0] ^ '0' ^ '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            break;
        case CTRL_Q:
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("\n\nStopping the kernel space tic-tac-toe game...\n");
            break;
        case KEY_Q:
            *tab = XO_TAB_RECORD;
            break;
        case KEY_W:
            *tab = XO_TAB_LOADAVG;
            break;
        }
    }

    close(attr_fd);
}

static void task_print_now(int argc, void *argv[])
{
    (void) argc;
    (void) argv;
    print_now();
}

static void task_io(int argc, void *argv[])
{
    (void) argc;
    co_chan *chan = argv[0];
    struct task_obj *tobj = (struct task_obj *) argv[1];
    int device_fd = *(int *) argv[2];
    int max_fd = *(int *) argv[3];
    struct xo_table xo_tlb;
    fd_set readset;
    int wait_fd = read_attr ? max_fd : STDIN_FILENO;

    FD_ZERO(&readset);
    FD_SET(STDIN_FILENO, &readset);
    if (read_attr)
        FD_SET(device_fd, &readset);

    int result = select(wait_fd + 1, &readset, NULL, NULL, NULL);
    if (result < 0) {
        printf("Error with select system call\n");
        exit(1);
    }

    if (FD_ISSET(STDIN_FILENO, &readset)) {
        FD_CLR(STDIN_FILENO, &readset);
        listen_keyboard_handler(&tobj->tab);
    } else if (read_attr && FD_ISSET(device_fd, &readset)) {
        FD_CLR(device_fd, &readset);
        read(device_fd, &xo_tlb, sizeof(struct xo_table));

        save_xy();
        update_table(&xo_tlb);
        restore_xy();
        tobj->tlb = xo_tlb;
    }
    stop_message(!read_attr);

    /* Send pointer to tobj through channel */
    void *ptr = tobj;
    co_chan_send(chan, &ptr);
}

static void task_tab(int argc, void *argv[])
{
    (void) argc;
    co_chan *chan = argv[0];
    void *ptr = NULL;
    co_chan_recv(chan, &ptr);
    struct task_obj *tobj = ptr;
    if (read_attr)
        tui_update_tab(tobj->tab, &tobj->tlb);
}

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    if (!status_check())
        exit(1);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    if (device_fd < 0) {
        perror("Failed to open " XO_DEVICE_FILE);
        exit(1);
    }
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;
    char *logo = load_logo("logof.txt");
    tui_init(device_fd);

    clean_screen();
    render_logo(logo);
    render_boards_temp(N_GAMES);

    co_sched_init();

    struct task_obj tobj;
    tobj.tab = XO_TAB_RECORD;
    co_chan *chan = co_chan_make(sizeof(void *), 0x10);
    while (!end_attr) {
        co_start(task_print_now, 0);
        co_start(task_io, 4, chan, &tobj, &device_fd, &max_fd);
        co_start(task_tab, 1, chan);
        co_run();
        outbuf_flush();
    }
    co_chan_release(chan);
    tui_quit();
    fcntl(STDIN_FILENO, F_SETFL, flags);
    free(logo);
    close(device_fd);

    return 0;
}
