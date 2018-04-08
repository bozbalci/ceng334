#ifndef PHGAME_H
#define PHGAME_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// #define DEBUG

#define POLL_NOTIMEOUT 0
#define CLIENT_RDY(i) fds[(i)].revents & POLLIN
#define SKIP_DEAD(i) if (!server_clientalive(&grid->clients[(i)])) \
                         continue;

#ifdef DEBUG
#define LOG(fmt, args...) fprintf(stderr, fmt, ## args)
#else
#define LOG(fmt, args...)
#endif

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

typedef struct
{
    Coordinate move_request;
} ClientMsg;

typedef enum
{
    CT_HUNTER,
    CT_PREY
} ClientType;

typedef struct
{
    ClientType type;
    Coordinate pos;
    int energy;
    int alive;
} UnitInfo;

typedef struct
{
    pid_t pid;
    int fd;
    int idx;
    UnitInfo ui;
} Client;

typedef struct
{
    Coordinate mapsize;
    int num_obstacles;
    int num_clients;
    Coordinate *obstacles;
    Client *clients;
} Grid;

ServerMsg servermsg_new(Grid *, Client *);
ServerMsg servermsg_recv(void);
ssize_t servermsg_send(Client *, ServerMsg);
ClientMsg clientmsg_new(ServerMsg, ClientType, Coordinate);
ClientMsg clientmsg_recv(Client *);
ssize_t clientmsg_send(ClientMsg);
void client_main(ClientType, Coordinate);
void client_randsleep(void);
void grid_destroy(Grid *);
int grid_distance(Coordinate, Coordinate);
int grid_equal(Coordinate, Coordinate);
Grid *grid_fromfmt(void);
void grid_neighbors(Coordinate *, int *, Coordinate, Coordinate);
void grid_print(Grid *);
int ipc_createpipe(int *);
int ipc_packintarg(char *, int);
void ipc_closeclientend(int *);
void ipc_execclient(ClientType, Coordinate);
void ipc_redirstdio(int *);
void ipc_setcloexec(int *);
void server_main(void);
void server_processmsg(int *, struct pollfd *, Grid *, Client *, ClientMsg);
int server_isstable(Grid *);
void server_forkclient(Client *, Coordinate);
void server_linkclient(Client *, pid_t, int *);
void server_killclient(Client *);
int server_clientalive(Client *);
ClientType server_clientadvtype(Client *);
Coordinate server_clientnearestadv(Grid *grid, Client *);
void server_clientobjects(Coordinate *, int *, Grid *, Client *);

// servermsg_new - create a new ServerMsg response for a client
//     grid: the grid object
//     client: client asking for the data
//
// Calculates the ServerMsg fields as required, and packs them into a
// struct; and returns it.
ServerMsg
servermsg_new(Grid *grid, Client *client)
{
    Coordinate object_pos[4];
    int object_count, i;
    ServerMsg msg;

    // We memset() here to avoid complaints by Valgrind on using write() on
    // an uninitialized buffer. The object_pos[] array is not completely
    // initialized, and can lead to undefined behavior if not checked.
    memset(&msg, 0, sizeof(ServerMsg));

    msg.pos = client->ui.pos;
    msg.adv_pos = server_clientnearestadv(grid, client);
    server_clientobjects(object_pos, &object_count, grid, client);
    msg.object_count = object_count;

    // Copy the local array into the struct.
    for (i = 0; i < object_count; i++)
        msg.object_pos[i] = object_pos[i];

    return msg;
}

// servermsg_recv - read a ServerMsg from the standard input
//
// Reads a ServerMsg from standard input and returns it.
// On error, prints the reason on stderr and exits with a failure code.
ServerMsg
servermsg_recv(void)
{
    ServerMsg msg;
    ssize_t nbytes;
    
    nbytes = read(0, &msg, sizeof(ServerMsg));
    
    if (nbytes < 0)
    {
        perror("servermsg_recv");
        exit(EXIT_FAILURE);
    }

    return msg;
}

// servermsg_send - send a ServerMsg to a bidirectional pipe
//     client: The client to send the message to.
//     msg: The message.
//
// Sends a ServerMsg through the file descriptor of a client.
// On error, prints the reason on stderr and exits with a failure code.
ssize_t
servermsg_send(Client *client, ServerMsg msg)
{
    ssize_t nbytes;
    
    nbytes = write(client->fd, &msg, sizeof(ServerMsg));

    if (nbytes < 0)
    {
        perror("servermsg_send");
        exit(EXIT_FAILURE);
    }
    
    return nbytes;
}

// clientmsg_new - create a new ClientMsg response to the server 
//     msgin: The message received from the server.
//     type: Hunter or prey.
//     mapsize: The dimensions of the map.
//
// Creates a new ClientMsg based on the ServerMsg and the client's own
// type. On error, prints the reason on stderr and exits with a
// failure code.
ClientMsg
clientmsg_new(ServerMsg msgin, ClientType type, Coordinate mapsize)
{
    ClientMsg msg;
    Coordinate neighbors[4], result;
    int num_neighbors, i, j, valid, curdistance, newdistance;

    // Get the neighbors into the local array, and store the number of
    // neighbors in num_neighbors.
    grid_neighbors(neighbors, &num_neighbors, mapsize, msgin.pos);

    // The distance between this unit and the nearest adversary.
    curdistance = grid_distance(msgin.pos, msgin.adv_pos);

    for (i = 0; i < num_neighbors; i++)
    {
        result = neighbors[i];

        for (j = 0; j < msgin.object_count; j++)
            if (grid_equal(result, msgin.object_pos[j]))
                goto obstructed;

        newdistance = grid_distance(result, msgin.adv_pos);

        // A move for a hunter is valid if it is closer to its adversary
        // after a move (it is *chasing* the adversary), meaning that the
        // distance must be less than the initial distance. The opposite
        // holds true for preys.
        if (type == CT_HUNTER)
            valid = newdistance < curdistance;
        else
            valid = newdistance > curdistance;

        // No need to search for other moves if the current one is valid.
        if (valid)
            break;

obstructed: // Continue the outer for-loop.
        ;
    }

    // If no valid moves were found, request to stay in the current
    // position.
    if (!valid)
        result = msgin.pos;
    msg.move_request = result;

    return msg;
}

// clientmsg_recv - read a ClientMsg from a bidirectional pipe
//     client: The client to read the message from.
//
// Reads a ClientMsg from the specified client's file descriptor.
// On error, prints the reason on stderr and exits with a failure code.
ClientMsg
clientmsg_recv(Client *client)
{
    ClientMsg msg;
    ssize_t nbytes;
    
    nbytes = read(client->fd, &msg, sizeof(ClientMsg));

    if (nbytes < 0)
    {
        perror("clientmsg_recv");
        exit(EXIT_FAILURE);
    }

    return msg;
}

// clientmsg_recv - send a ClientMsg to the standard output
//     msg: The message to send.
//
// Writes a ClientMsg to the standard output. On error, prints the
// reason on stderr and exits with a failure code.
ssize_t
clientmsg_send(ClientMsg msg)
{
    ssize_t nbytes;
    
    nbytes = write(1, &msg, sizeof(ClientMsg));

    if (nbytes < 0)
    {
        perror("clientmsg_send");
        exit(EXIT_FAILURE);
    }

    return nbytes;
}

// client_main - the main client loop
//     type: Hunter or prey.
//     mapsize: The size of the map.
//
// When the client process enters this function, it will enter a loop that
// consists of the following steps:
//
//     1. read a ServerMsg from standard input
//     2. calculate a corresponding ClientMsg
//     3. write the calculated ClientMsg to standard output
//     4. sleep for a random amount of time
//
// This loop can only be broken by a SIGTERM by the server process.
void
client_main(ClientType type, Coordinate mapsize)
{
    ClientMsg msgout;
    ServerMsg msgin;

    while (1)
    {
        msgin = servermsg_recv();
        msgout = clientmsg_new(msgin, type, mapsize);
        clientmsg_send(msgout);
        client_randsleep();
    }
}

// client_randsleep - sleep for a random amount of time
//
// Sleeps for a random amount of time between 10ms and 100ms.
void client_randsleep(void)
{
    usleep(10000 * (1 + rand() % 9));
}

// grid_destroy - deallocates a grid object containing its data
//     grid: The grid to destroy.
//
// The data structures of the server are deallocated with this function,
// including the Client array and the obstacle array.
void grid_destroy(Grid *grid)
{
    if (!grid)
        return;

    free(grid->obstacles);
    free(grid->clients);
    free(grid);
}

// grid_distance - the distance between two coordinates
//     a: The first coordinate.
//     b: The first coordinate.
//
// Returns the Manhattan Distance between two points on the grid.
int
grid_distance(Coordinate a, Coordinate b)
{
    return abs(a.x - b.x) + abs(a.y - b.y);
}

// grid_equal - equality of two coordinates
//     a: The first coordinate.
//     b: The first coordinate.
//
// Returns true if the two coordinates are equal, false otherwise.
int
grid_equal(Coordinate a, Coordinate b)
{
    return (a.x == b.x) && (a.y == b.y);
}

// grid_fromfmt - parse a grid from standard input
//
// Allocates and populates the necessary memory space for the grid,
// using the values parsed from the standard input. The format is defined
// in the homework text.
Grid *
grid_fromfmt(void)
{
    Client client;
    const char *fmt_coord = "%d %d";
    const char *fmt_quantity = "%d";
    const char *fmt_unit = "%d %d %d";
    Coordinate coord;
    Grid *grid;
    int a, b, c, i;
    UnitInfo ui;

    grid = malloc(sizeof(Grid));

    // <width> <height>
    scanf(fmt_coord, &a, &b);
    grid->mapsize.x = a;
    grid->mapsize.y = b;

    // <num_obstacles>
    scanf(fmt_quantity, &a);
    grid->num_obstacles = a;
    grid->obstacles = malloc(a * sizeof(Coordinate));

    // <x> <y>
    for (i = 0; i < grid->num_obstacles; i++)
    {
        scanf(fmt_coord, &a, &b);
        coord.x = a;
        coord.y = b;
        grid->obstacles[i] = coord;
    }
    
    // <num_hunters>
    scanf(fmt_quantity, &a);
    grid->num_clients = a;
    grid->clients = malloc(a * sizeof(Client));

    // <x> <y> <energy>
    for (i = 0; i < grid->num_clients; i++)
    {
        scanf(fmt_unit, &a, &b, &c);
        coord.x = a;
        coord.y = b;
        ui.type = CT_HUNTER;
        ui.pos = coord;
        ui.energy = c;
        ui.alive = 1;
        client.ui = ui;
        grid->clients[i] = client;
    }
    
    // <num_preys>
    scanf(fmt_quantity, &a);
    grid->num_clients += a;
    grid->clients = realloc(grid->clients,
        grid->num_clients * sizeof(Client));
    
    // <x> <y> <energy>
    for (; i < grid->num_clients; i++)
    {
        scanf(fmt_unit, &a, &b, &c);
        coord.x = a;
        coord.y = b;
        ui.type = CT_PREY;
        ui.pos = coord;
        ui.energy = c;
        ui.alive = 1;
        client.ui = ui;
        grid->clients[i] = client;
    }
            
    return grid;
}

// grid_neighbors - the neighboring cells of a cell
//     buf: A preallocated array of Coordinates to fill with the result.
//     num_neighbors: An integer to hold the number of neighbors found.
//     mapsize: The size of the map.
//     pos: The cell to search whose neighboring cells.
void
grid_neighbors(Coordinate *buf, int *num_neighbors, Coordinate mapsize,
    Coordinate pos)
{
    Coordinate coord;
    int width = mapsize.x, height = mapsize.y, i = 0;

    // Top cell
    coord.x = pos.x + 1;
    coord.y = pos.y;
    if (coord.x < height)
    {
        buf[i] = coord;
        i++;
    }

    // Left cell
    coord.x = pos.x;
    coord.y = pos.y - 1;
    if (coord.y >= 0)
    {
        buf[i] = coord;
        i++;
    }
    
    // Bottom cell
    coord.x = pos.x - 1;
    coord.y = pos.y;
    if (coord.x >= 0)
    {
        buf[i] = coord;
        i++;
    }
    
    // Right cell
    coord.x = pos.x;
    coord.y = pos.y + 1;
    if (coord.y < width)
    {
        buf[i] = coord;
        i++;
    }

    *num_neighbors = i;
}

// grid_print - print a grid to standard output
//     grid: The grid to print.
//
// Prints a grid to the standard output. The format is specified in the
// homework text.
void
grid_print(Grid *grid)
{
    Coordinate c;
    int i, j, k;

    // Top left plus
    putchar('+');

    // Top edge
    for (i = 0; i < grid->mapsize.x; i++)
        putchar('-');

    // Top right plus
    putchar('+');

    // End of first line
    putchar('\n');

    for (i = 0; i < grid->mapsize.y; i++)
    {
        // Left edge
        putchar('|');

        for (j = 0; j < grid->mapsize.x; j++)
        {
            c.x = i;
            c.y = j;

            for (k = 0; k < grid->num_obstacles; k++)
                if (grid_equal(c, grid->obstacles[k]))
                {
                    putchar('X'); // Obstacle
                    goto found;
                }

            for (k = 0; k < grid->num_clients; k++)
            {
                SKIP_DEAD(k);

                if (grid_equal(c, grid->clients[k].ui.pos))
                {
                    if (grid->clients[k].ui.type == CT_HUNTER)
                    {
                        putchar('H'); // Hunter
                        goto found;
                    }
                    else
                    {
                        putchar('P'); // Prey
                        goto found;
                    }
                }
            }

            // No object was found at coordinate.
            putchar(' ');

found: // Found an item to print, so skip to the next coordinate.
            ;
        }

        // Right edge
        putchar('|');
        putchar('\n');
    }
    
    // Bottom left plus
    putchar('+');

    // Bottom edge
    for (i = 0; i < grid->mapsize.x; i++)
        putchar('-');

    // Bottom right plus
    putchar('+');

    // End of last line
    putchar('\n');
    fflush(stdout);

#ifdef DEBUG
    for (i = 0; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        LOG("client %d of type %d at (%d, %d), energy = %d\n",
                i,
                grid->clients[i].ui.type,
                grid->clients[i].ui.pos.x,
                grid->clients[i].ui.pos.y,
                grid->clients[i].ui.energy
        );
    }
#endif
}

// ipc_createpipe - create a bidirectional pipe
//     fd: A two-element array of file descriptors to set.
//
// Creates a socket that acts similarly to a bidirectional pipe. Stores
// the file descriptors in the variable fd.
int
ipc_createpipe(int *fd)
{
    return socketpair(AF_UNIX, SOCK_STREAM, PF_UNIX, fd);
}

// ipc_packintarg - pack an integer into a string argument
//     buf: The preallocated string buffer to fill.
//     arg: The integer to convert into a string.
//
// Takes an integer and returns a string containing the integer.
// Required for executing child processes with integer arguments.
int
ipc_packintarg(char *buf, int arg)
{
    sprintf(buf, "%d", arg);
}

// ipc_closeclientend - close the client end of a bidirectional pipe
//     fd: The file descriptor pair.
//
// Takes a bidirectional pipe, and (locally) closes the client end
// of the pipe, so that other processes do not inherit it when forking.
void
ipc_closeclientend(int *fd)
{
    close(fd[1]);
}

// ipc_execclient - executes the client processes
//     type: Hunter or prey.
//     mapsize: The size of the map.
//
// Packs the map size into string arguments and calls the correct
// executable for the client process.
void
ipc_execclient(ClientType type, Coordinate mapsize)
{
    char arg1[32], arg2[32];

    ipc_packintarg(arg1, mapsize.x);
    ipc_packintarg(arg2, mapsize.y);

    if (type == CT_HUNTER)
        execl("./hunter", "hunter", arg1, arg2, (char *) NULL);
    else
        execl("./prey", "prey", arg1, arg2, (char *) NULL);

    perror("execl"); // execel returns only on error
    _exit(EXIT_FAILURE);
}

// ipc_redirstdio - redirect stdio to socket
//     fd: The file descriptor pair.
//
// Redirects standard input and output to the specified socket.
void
ipc_redirstdio(int *fd)
{
    dup2(fd[1], 0);
    dup2(fd[1], 1);
}

// ipc_setcloexec - mark file descriptors with close-on-exec
//     fd: The file descriptor pair.
//
// Marks both ends of the specified file descriptor pair with the
// close-on-exec flag, so that they are not inherited when client
// processes are launched.
void
ipc_setcloexec(int *fd)
{
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
}

// server_main - the main loop of the server process
//
// This function is responsible for initializing the grid, the clients,
// forking the clients, and setting up the file descriptors. After all
// these tasks have been completed, it will send initial messages to
// all client messages and enter an event loop. Inside this event loop,
// it will read requests from ready clients and serve their requests,
// signaling other processes as required. The loop continues until
// one type of adversaries have been defeated.
void
server_main(void)
{
    ClientMsg msgin;
    Grid *grid;
    int i, grid_updated = 0;
    ServerMsg msgout;
    struct pollfd *fds;

    // Parse and print the grid.
    grid = grid_fromfmt();
    grid_print(grid);

    // Allocate the file descriptor table for polling.
    fds = malloc(grid->num_clients * sizeof(struct pollfd));

    for (i = 0; i < grid->num_clients; i++)
    {
        // Fork a new client process.
        server_forkclient(&grid->clients[i], grid->mapsize);

        // We assign an unique index to every client process, this helps
        // us in a few different ways we'll see later.
        grid->clients[i].idx = i;

        // Prepare and send the initial message for the client process.
        msgout = servermsg_new(grid, &grid->clients[i]);
        servermsg_send(&grid->clients[i], msgout);

        // Save the file descriptors into the table data structure.
        // We are interested in the POLLIN event of each file descriptor.
        fds[i].fd = grid->clients[i].fd;
        fds[i].events = POLLIN;
    }

    // This is the main loop of the server.
    while (!server_isstable(grid))
    {
        poll(fds, grid->num_clients, POLL_NOTIMEOUT);

        for (i = 0; i < grid->num_clients; i++)
        {
            SKIP_DEAD(i);

            if (CLIENT_RDY(i))
            {
                msgin = clientmsg_recv(&grid->clients[i]);
                server_processmsg(&grid_updated, fds, grid,
                    &grid->clients[i], msgin);

                // We check that the process is still alive before
                // dispatching a response, because server_processmsg
                // may have killed it in some scenarios.
                if (server_clientalive(&grid->clients[i]))
                {
                    msgout = servermsg_new(grid, &grid->clients[i]);
                    servermsg_send(&grid->clients[i], msgout);
                }

                // Update the grid if necessary.
                if (grid_updated)
                {
                    grid_print(grid);
                    grid_updated = 0;
                }
            }
        }
    }

    // Take no prisoners -- kill all the remaining processes.
    for (i = 0; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        fds[i].fd = -1;
        server_killclient(&grid->clients[i]);
        LOG("[death] %d survived until the end\n", i);
    }

    free(fds);
    grid_destroy(grid);

    LOG("[server] exiting gracefully\n");
    exit(EXIT_SUCCESS);
}

// server_processmsg - process move requests
//     grid_updated: Set to 1 if the grid is updated.
//     fds: Required to switch off file descriptors of killed processes.
//     grid: The grid containing the information of all clients.
//     client: The client who triggered this function.
//     msg: The move request to process.
//
// Processes a client message. Checks for collisions, deaths due to
// collisions or the exhaustion of energy, transfers energy from preys to
// hunters on their death, updates the coordinates of the clients.
//
// This function also handles the killing of processes. When a process
// is killed in server_processmsg, server_killclient is called and the
// file descriptor is closed. Here, we switch off the value in the
// polling table to prevent socket errors.
void server_processmsg(int *grid_updated, struct pollfd *fds,
    Grid *grid, Client *client, ClientMsg msg)
{
    ClientType type, adv_type;
    Coordinate coord;
    int i, energy;

    coord = msg.move_request;
    type = client->ui.type;
    adv_type = server_clientadvtype(client);

    for (i = 0; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        if (grid_equal(coord, grid->clients[i].ui.pos))
        {
            // Types are equal => this is a collision with an ally.
            // This move is not allowed, so the request is denied.
            if (type == grid->clients[i].ui.type)
            {
                *grid_updated = 0;
                return;
            }
            else
            {
                // This is a collision of a hunter into a prey.
                // The prey shall be killed, and the hunter will gain
                // its energy. We still have to check if the hunter has
                // positive energy, as the prey can theoretically have
                // a negative or zero energy.
                if (type == CT_HUNTER)
                {
                    client->ui.pos = msg.move_request;
                    client->ui.energy--;
                    energy = grid->clients[i].ui.energy;
                    client->ui.energy += energy;

                    if (client->ui.energy <= 0)
                    {
                        fds[client->idx].fd = -1;
                        server_killclient(client);
                        LOG("[death] hunter %d exhausted\n", client->idx);
                    }

                    fds[i].fd = -1;
                    server_killclient(&grid->clients[i]);
                    LOG("[death] %d killed by hunter %d\n", i, client->idx);

                    *grid_updated = 1;
                    return;
                }
                else
                {
                    // This (unfortunate) case occurs when a prey walks
                    // into a hunter. In this case, we need to kill the
                    // triggering client and yield its energy to the
                    // killing hunter.
                    client->ui.pos = msg.move_request;
                    energy = client->ui.energy;
                    grid->clients[i].ui.energy += energy;

                    fds[client->idx].fd = -1;
                    server_killclient(client);
                    LOG("[death] prey %d fed hunter %d\n", client->idx, i);

                    *grid_updated = 1;
                    return;
                }
            }
        }
    }

    // This is the catch-all code for all movements without collisions.
    if (grid_equal(client->ui.pos, msg.move_request))
    {
        // The move request is the same location as the current location,
        // therefore the grid hasn't been updated and energy will not be
        // lost. Return immediately.
        *grid_updated = 0;
        return;
    }

    // Otherwise, update the position and remove 1 energy if the currently
    // moving unit is a hunter.
    client->ui.pos = msg.move_request;
    if (type == CT_HUNTER)
        client->ui.energy--;
    if (client->ui.energy <= 0)
    {
        fds[client->idx].fd = -1;
        server_killclient(client);
        LOG("[death] hunter %d exhausted\n", client->idx);
    }
    *grid_updated = 1;
}

// server_isstable - the end condition of the simulation
//     grid: Grid containing all of the client information.
//
// Returns true if either preys or hunters have been defeated.
int
server_isstable(Grid *grid)
{
    int num_hunters = 0, num_preys = 0, i;

    for (i = 0; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        if (grid->clients[i].ui.type == CT_HUNTER)
            num_hunters++;
        else
            num_preys++;
    }

    return (num_hunters == 0) && (num_preys >= 0) ||
           (num_preys == 0) && (num_hunters >= 0);
}

// server_forkclient - fork a new process and link a client
//     client: The client to link to the new process.
//     mapsize: The size of the map.
//
// Takes an unpopulated Client object and fills it with a new pid
// and a file descriptor; belonging to the new client process.
// The child process then proceeds to exec its own executable.
void
server_forkclient(Client *client, Coordinate mapsize)
{
    int fd[2];
    pid_t pid;

    ipc_createpipe(fd);
    ipc_setcloexec(fd);

    pid = fork();

    if (pid) // Server code.
    {
        server_linkclient(client, pid, fd);
        ipc_closeclientend(fd);
    }
    else // Client code.
    {
        ipc_redirstdio(fd);
        ipc_execclient(client->ui.type, mapsize);
    }
}

// server_linkclient - attach a pid and fd to a client
//     client: The client to attach process information to.
//     pid: Process ID of the child process.
//     fd: Bidirectional pipe for the new connection.
//
// Populates a Client object with PID and a file descriptor,
// so that the server process can establish connection to it after
// fork-execing.
void
server_linkclient(Client *client, pid_t pid, int *fd)
{
    client->pid = pid;
    client->fd = fd[0];
}

// server_killclient - kill a client
//     client: The client to kill.
//
// Sends a signal to kill the process associated with the specified
// client object, and sets its alive flag to zero to indicate death.
// Also closes the server-end of the bidirectional pipe that was
// created when the process was created.
void
server_killclient(Client *client)
{
    int status;

    client->ui.alive = 0;
    kill(client->pid, SIGTERM);
    wait(&status);
    close(client->fd);
}

// server_clientalive - check if client is alive
//     client: The client to check.
//
// Returns true if the client is alive.
int
server_clientalive(Client *client)
{
    return client->ui.alive;
}

// server_clientadvtype - the type of a client's adversary
//     client: The client to query.
//
// Returns CT_PREY for hunters, CT_HUNTER for preys.
ClientType
server_clientadvtype(Client *client)
{
    if (client->ui.type == CT_HUNTER)
        return CT_PREY;

    return CT_HUNTER;
}

// server_clientnearestadv - nearest adversary of client
//     grid: The grid containing all of the clients' information.
//     client: The client whose nearest adversary is asked.
//
// Returns the coordinates of the nearest unit of type CT_HUNTER
// if client is a prey, CT_PREY otherwise.
Coordinate
server_clientnearestadv(Grid *grid, Client *client)
{
    ClientType adv_type;
    Coordinate result;
    int i, distance, mindistance;

    adv_type = server_clientadvtype(client);

    // Find the distance with the first adversary on the map.
    for (i = 0; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        if (adv_type != grid->clients[i].ui.type)
            continue;

        mindistance = grid_distance(client->ui.pos,
            grid->clients[i].ui.pos);
        result = grid->clients[i].ui.pos;
    }

    // Compare the remaining adversaries to find the minimum
    // distance.
    for (; i < grid->num_clients; i++)
    {
        SKIP_DEAD(i);

        if (adv_type != grid->clients[i].ui.type)
            continue;

        distance = grid_distance(client->ui.pos,
            grid->clients[i].ui.pos);

        if (distance < mindistance)
        {
            mindistance = distance;
            result = grid->clients[i].ui.pos;
        }
    }

    return result;
}

// server_clientobjects - objects neighboring a certain client
//     buf: Preallocated array of coordinates to fill with the result.
//     num_objects: Holds the number of objects neighboring the client.
//     grid: The grid containing the information on all of the clients.
//     client: The client whose neighboting objects are queried.
//
// For every neighboring cell of the client, we look for either an
//
//     1. allied unit
//     2. obstacle
//
// and we collect them into buf. The length of buf is stored in
// num_objects.
void
server_clientobjects(Coordinate *buf, int *num_objects, Grid *grid,
        Client *client)
{
    ClientType adv_type;
    Coordinate neighbors[4], x, y;
    int num_neighbors, i, j, k = 0;

    adv_type = server_clientadvtype(client);
    grid_neighbors(neighbors, &num_neighbors, grid->mapsize,
        client->ui.pos);

    for (i = 0; i < num_neighbors && k <= num_neighbors; i++)
    {
        for (j = 0; j < grid->num_obstacles; j++)
        {
            x = grid->obstacles[j];
            y = neighbors[i];

            if (grid_equal(x, y))
            {
                buf[k] = x;
                k++;

                LOG("(%d, %d) found obstacle (%d, %d) as obstacle\n",
                    client->ui.pos.x, client->ui.pos.y,
                    x.x, x.y);

                goto found;
            }
        }

        for (j = 0; j < grid->num_clients; j++)
        {
            SKIP_DEAD(j);

            // If a neighboring client is an enemy, it is not considered
            // an obstacle.
            if (grid->clients[j].ui.type == adv_type)
                continue;
            
            x = grid->clients[j].ui.pos;
            y = neighbors[i];

            if (grid_equal(x, y))
            {
                buf[k] = x;
                k++;

                LOG("(%d, %d) found ally (%d, %d) as obstacle\n",
                    client->ui.pos.x, client->ui.pos.y,
                    x.x, x.y);

                goto found;
            }
        }
found: // Found an object, so we can skip to the next neighboring cell.
        ;
    }

    *num_objects = k;
}

#endif // PHGAME_H
