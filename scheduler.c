#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define MAX_DOCKS 100
#define MAX_SOLVERS 100

typedef struct {
    int id;
    int category;
} Dock;

typedef struct {
    long mtype;
    int timestep;
    int numShipRequests;
    int dockId;
    int shipId;
    int direction;
    int craneId;
    int cargoId;
    char padding[200];
} MessageStruct;

typedef struct {
    int dockId;
    int numCranes;
    int craneCapacities[10];
    int maxShipCategory;
    int isOccupied;
    int shipId;
    int shipDirection;
    int cargoHandled;
    int cargoTotal;
} DockStatus;

typedef struct {
    int shipId;
    int direction;
    int shipCategory;
    int numCargoItems;
    int cargoWeights[10];
    int maxWaitingTime;
    int currentWaitingTime;
    char authString[10];
} ShipRequest;

typedef struct {
    int timestep;
    int numShipRequests;
    ShipRequest newShipRequests[100];
    DockStatus dockStatuses[100];
    char authStrings[100][10];
} MainSharedMemory;

typedef struct {
    long mtype;
    int dockId;
    char authStringGuess[10];
} SolverRequest;

typedef struct {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char inputPath[256];
    snprintf(inputPath, sizeof(inputPath), "testcase%s/input.txt", argv[1]);

    FILE* file = fopen(inputPath, "r");
    if (!file) {
        perror("Failed to open input.txt");
        exit(EXIT_FAILURE);
    }

    key_t shmKey, mainMsgQueueKey, solverQueueKeys[MAX_SOLVERS];
    int numSolvers, numDocks;
    Dock docks[MAX_DOCKS];

    fscanf(file, "%d", &shmKey);
    fscanf(file, "%d", &mainMsgQueueKey);
    fscanf(file, "%d", &numSolvers);
    for (int i = 0; i < numSolvers; i++) {
        fscanf(file, "%d", &solverQueueKeys[i]);
    }

    fscanf(file, "%d", &numDocks);
    for (int i = 0; i < numDocks; i++) {
        fscanf(file, "%d %d", &docks[i].id, &docks[i].category);
    }

    fclose(file);

    printf("✔️ input.txt read successfully from %s\n", inputPath);
    printf("Shared Memory Key: %d\n", shmKey);
    printf("Main Message Queue Key: %d\n", mainMsgQueueKey);
    printf("Number of Solvers: %d\n", numSolvers);
    for (int i = 0; i < numSolvers; i++) {
        printf("Solver %d Queue Key: %d\n", i, solverQueueKeys[i]);
    }
    printf("Number of Docks: %d\n", numDocks);
    for (int i = 0; i < numDocks; i++) {
        printf("Dock ID: %d, Category: %d\n", docks[i].id, docks[i].category);
    }

    int shmId = shmget(shmKey, sizeof(MainSharedMemory), 0666);
    if (shmId == -1) {
        perror("Failed to get shared memory segment");
        exit(EXIT_FAILURE);
    }

    MainSharedMemory* sharedMemory = (MainSharedMemory*)shmat(shmId, NULL, 0);
    if (sharedMemory == (void*)-1) {
        perror("Failed to attach shared memory");
        exit(EXIT_FAILURE);
    }

    int mainMsgQueueId = msgget(mainMsgQueueKey, 0666);
    if (mainMsgQueueId == -1) {
        perror("Failed to get main message queue");
        exit(EXIT_FAILURE);
    }

    int solverQueueIds[MAX_SOLVERS];
    for (int i = 0; i < numSolvers; i++) {
        solverQueueIds[i] = msgget(solverQueueKeys[i], 0666);
        if (solverQueueIds[i] == -1) {
            fprintf(stderr, "Failed to get solver message queue %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    printf("✔️ IPC mechanisms set up successfully.\n");
    printf("Scheduler setup complete. Ready to start scheduling loop...\n");

    return 0;
}

