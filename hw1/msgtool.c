#include <stdio.h>
#include <unistd.h>

typedef struct
{
    int x;
    int y;
} Coordinate;

typedef struct
{
    Coordinate pos;
    Coordinate adv_pos;
    int object_count;
    Coordinate object_pos[4];
} ServerMsg;

int main(int argc, char **argv)
{
    ServerMsg msg;
    Coordinate coord;
    int a, b, c, i;

    fprintf(stderr, "pos (%%d %%d): ");
    scanf("%d %d", &a, &b);
    coord.x = a;
    coord.y = b;
    msg.pos = coord;
    
    fprintf(stderr, "adv_pos (%%d %%d): ");
    scanf("%d %d", &a, &b);
    coord.x = a;
    coord.y = b;
    msg.adv_pos = coord;
    
    fprintf(stderr, "object_count (%%d): ");
    scanf("%d", &a);
    msg.object_count = a;
    c = a;

    for (i = 0; i < c; i++)
    {
        fprintf(stderr, "object_pos[%d] (%%d %%d): ", i);
        scanf("%d %d", &a, &b);
        coord.x = a;
        coord.y = b;
        msg.object_pos[i] = coord;
    }

    write(1, &msg, sizeof(ServerMsg));
    return 0;
}
