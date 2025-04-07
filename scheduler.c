#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define MAX_DOCKS 30
#define MAX_SOLVERS 8
#define MAX_AUTH_STRING_LEN 100
#define MAX_NEW_REQUESTS 100
#define MAX_CARGO_COUNT 200

typedef struct {
    int id;
    int category;
} Dock;

typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
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

typedef struct ShipRequest {
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory {
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
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

int compareShipRequests(const void* a, const void* b) {
    ShipRequest* reqA = (ShipRequest*)a;
    ShipRequest* reqB = (ShipRequest*)b;

    if (reqA->emergency != reqB->emergency) {
        return reqB->emergency - reqA->emergency;
    }
    if (reqA->timestep != reqB->timestep) {
        return reqA->timestep - reqB->timestep;
    }
    return reqA->shipId - reqB->shipId;
}

void sortShipRequests(ShipRequest requests[], int count) {
    qsort(requests, count, sizeof(ShipRequest), compareShipRequests);
}

int findSuitableDock(DockStatus docks[], int numDocks, ShipRequest* ship) {
    for (int i = 0; i < numDocks; i++) {
        if (!docks[i].isOccupied && docks[i].maxShipCategory >= ship->category) {
            return i;
        }
    }
    return -1;
}

void assignShipToDock(DockStatus* dock, ShipRequest* ship) {
    dock->isOccupied = 1;
    dock->shipId = ship->shipId;
    dock->shipDirection = ship->direction;
    dock->cargoHandled = 0;
    dock->cargoTotal = ship->numCargo;
}

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
    while (fgetc(file) != '\n' && !feof(file));

    DockStatus dockStatuses[MAX_DOCKS];
    for (int i = 0; i < numDocks; i++) {
        dockStatuses[i].dockId = i + 1;
        dockStatuses[i].isOccupied = 0;
        dockStatuses[i].shipId = -1;
        dockStatuses[i].shipDirection = -1;
        dockStatuses[i].cargoHandled = 0;
        dockStatuses[i].cargoTotal = 0;

        char line[256];
        if (!fgets(line, sizeof(line), file)) {
            fprintf(stderr, "Error reading dock line %d\n", i + 1);
            exit(1);
        }

        char* token = strtok(line, " \t\n");
        int count = 0;
        while (token) {
            int val = atoi(token);
            if (count == 0) {
                dockStatuses[i].maxShipCategory = val;
            } else {
                dockStatuses[i].craneCapacities[count - 1] = val;
            }
            count++;
            token = strtok(NULL, " \t\n");
        }
        dockStatuses[i].numCranes = count - 1;
    }
    fclose(file);

    int shmId = shmget(shmKey, sizeof(MainSharedMemory), IPC_CREAT | 0666);
    if (shmId == -1) {
        perror("❌ Failed to create/get shared memory segment");
        exit(EXIT_FAILURE);
    }

    MainSharedMemory* sharedMemory = (MainSharedMemory*)shmat(shmId, NULL, 0);
    if (sharedMemory == (void*)-1) {
        perror("❌ Failed to attach shared memory");
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

    printf("🚢 Starting scheduler loop...\n");

    while (1) {
        MessageStruct validationMsg;
        ssize_t msgSize = sizeof(MessageStruct) - sizeof(long);
        if (msgrcv(mainMsgQueueId, &validationMsg, msgSize, 1, 0) == -1) {
            perror("❌ Failed to receive message from validation");
            exit(EXIT_FAILURE);
        }

        int timestep = validationMsg.timestep;
        int numRequests = validationMsg.numShipRequests;

        if (numRequests == 0 && timestep == -1) {
            printf("🛑 Simulation end signal received.\n");
            break;
        }

        printf("\n⏳ Timestep: %d | Ship Requests Received: %d\n", timestep, numRequests);

        ShipRequest incomingRequests[MAX_NEW_REQUESTS];
        for (int i = 0; i < numRequests; i++) {
            incomingRequests[i] = sharedMemory->newShipRequests[i];
        }

        sortShipRequests(incomingRequests, numRequests);
        printf("📋 Sorted %d ship requests (Emergency → FCFS):\n", numRequests);

        for (int i = 0; i < numRequests; i++) {
            ShipRequest* req = &incomingRequests[i];
            printf("  🚢 Ship ID %d | Timestep %d | Emergency %d | Category %d\n", 
                   req->shipId, req->timestep, req->emergency, req->category);

            int dockIdx = findSuitableDock(dockStatuses, numDocks, req);
            if (dockIdx == -1) {
                printf("     ❌ No suitable dock available.\n");
                continue;
            }

            assignShipToDock(&dockStatuses[dockIdx], req);
            printf("     ✅ Assigned to Dock %d (Max Category %d)\n", 
                   dockStatuses[dockIdx].dockId, dockStatuses[dockIdx].maxShipCategory);
        }

        MessageStruct endMsg;
        endMsg.mtype = 5;
        if (msgsnd(mainMsgQueueId, &endMsg, 0, 0) == -1) {
            perror("❌ Failed to notify validation of timestep completion");
        } else {
            printf("✅ Timestep %d processing done\n", timestep);
        }
    }

    return 0;
}

