#if defined(PREY) && defined(HUNTER)
    #error "You cannot specify both -DPREY or -DHUNTER at the same time."
#endif

#if !(defined(PREY) || defined(HUNTER))
    #error "You must specify either -DPREY or -DHUNTER."
#endif

#include "phgame.h"

int
main(int argc, char **argv)
{
    Coordinate mapsize;
    ClientType type;

    if (argc < 3)
    {
        fprintf(stderr, "Call me with 3 arguments!\n");
        exit(EXIT_FAILURE);
    }

    mapsize.x = atoi(argv[1]);
    mapsize.y = atoi(argv[2]);
    
#ifdef HUNTER
    type = CT_HUNTER;
#else
    type = CT_PREY;
#endif

    client_main(type, mapsize);

    return 0;
}
