#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>

int main(int argc, char *argv[])
{
        int ret = 0;
        int fd;
        int buf[10] = {0};
        char *filename = NULL;

        if (argc != 2) {
                printf("Error Usage!\n"
                       "Usage %s filename 0:1\n"
                       ,argv[0]);
                ret = -1;
                goto error;
        }

        filename = argv[1];
        fd = open(filename, O_RDWR|O_NONBLOCK);
        if (fd == -1) {
                perror("open failed!\n");
                ret = -1;
                goto error;
        }

        while (1) {
                ret = read(fd, buf, 1);
                if (ret < 0) {
                        perror("read error");
                        goto error;
                } else {
                        printf("buf[0] = %d, ret = %d\n", buf[0], ret);
                        buf[0] = 0;
                }
        }

error:
        close(fd);
        return ret;
}
