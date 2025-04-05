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

void initializeDockStatuses(DockStatus dockStatuses[], Dock docks[], int numDocks) {
    for (int i = 0; i < numDocks; i++) {
        dockStatuses[i].dockId = docks[i].id;
        dockStatuses[i].maxShipCategory = docks[i].category;
        dockStatuses[i].numCranes = 0;
        dockStatuses[i].isOccupied = 0;
        dockStatuses[i].shipId = -1;
        dockStatuses[i].shipDirection = -1;
        dockStatuses[i].cargoHandled = 0;
        dockStatuses[i].cargoTotal = 0;
        for (int j = 0; j < 10; j++) {
            dockStatuses[i].craneCapacities[j] = 0;
        }
    }
}

void printShipRequests(ShipRequest shipRequests[], int numRequests) {
    for (int i = 0; i < numRequests; i++) {
        printf("Ship ID: %d, Direction: %d, Category: %d, Cargo Items: %d\n",
            shipRequests[i].shipId,
            shipRequests[i].direction,
            shipRequests[i].shipCategory,
            shipRequests[i].numCargoItems);
    }
}

void updateShipWaitingTimes(ShipRequest shipRequests[], int numRequests) {
    for (int i = 0; i < numRequests; i++) {
        shipRequests[i].currentWaitingTime++;
    }
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
    for (int i = 0; i < numDocks; i++) {
        fscanf(file, "%d %d", &docks[i].id, &docks[i].category);
    }

    fclose(file);

    printf("✔️ input.txt read successfully from %s\n", inputPath);
    printf("IPC setup starting...\n");

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

    DockStatus dockStatuses[MAX_DOCKS];
    initializeDockStatuses(dockStatuses, docks, numDocks);

    printf("✔️ IPC mechanisms set up successfully.\n");

    int timestep = 0;
    printf("🚢 Starting scheduling loop...\n");

    while (1) {
        timestep = sharedMemory->timestep;
        printf("⏳ Timestep: %d | Num Requests: %d\n", timestep, sharedMemory->numShipRequests);
        int numRequests = sharedMemory->numShipRequests;

        if (numRequests == 0 && timestep > 0) {
            printf("⏹️  No more ship requests. Terminating.\n");
            break;
        }

        ShipRequest activeShips[100];
        int count = 0;
        for (int i = 0; i < numRequests; i++) {
            ShipRequest* req = &sharedMemory->newShipRequests[i];
            activeShips[count++] = *req;
            
            printf("📦 Timestep %d: Received Ship ID=%d | Cat=%d | Dir=%d | Cargo=%d\n",
            timestep, req->shipId, req->shipCategory, req->direction, req->numCargoItems);
        }

        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if ((activeShips[i].shipCategory > activeShips[j].shipCategory) ||
                    (activeShips[i].shipCategory == activeShips[j].shipCategory &&
                     activeShips[i].currentWaitingTime < activeShips[j].currentWaitingTime)) {
                    ShipRequest temp = activeShips[i];
                    activeShips[i] = activeShips[j];
                    activeShips[j] = temp;
                }
            }
        }

        for (int i = 0; i < count; i++) {
            ShipRequest* ship = &activeShips[i];
            int assigned = 0;

            for (int j = 0; j < numDocks; j++) {
                DockStatus* dock = &dockStatuses[j];

                if (!dock->isOccupied && dock->maxShipCategory >= ship->shipCategory) {
                    dock->isOccupied = 1;
                    dock->shipId = ship->shipId;
                    dock->shipDirection = ship->direction;
                    dock->cargoTotal = ship->numCargoItems;
                    strncpy(sharedMemory->authStrings[dock->dockId], ship->authString, 10);

                    MessageStruct msg;
                    msg.mtype = 1;
                    msg.timestep = timestep;
                    msg.numShipRequests = 1;
                    msg.dockId = dock->dockId;
                    msg.shipId = ship->shipId;
                    msg.direction = ship->direction;
                    msg.craneId = -1;  // will be used later
                    msg.cargoId = -1;  // will be used later
                    memset(msg.padding, 0, sizeof(msg.padding));

                    if (msgsnd(mainMsgQueueId, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
                        perror("Failed to send docking message");
                    } else {
                        printf("🛳️  Timestep %d: Docked ship %d at dock %d (category %d)\n",
                               timestep, ship->shipId, dock->dockId, ship->shipCategory);
                    }

                    assigned = 1;
                    break;
                }
            }

            if (!assigned) {
                printf("⌛ Timestep %d: Ship %d is waiting (category %d)\n",
                       timestep, ship->shipId, ship->shipCategory);
            }
        }

        updateShipWaitingTimes(sharedMemory->newShipRequests, sharedMemory->numShipRequests);
        
        MessageStruct endOfTimestepMsg;
        endOfTimestepMsg.mtype = 5;

        if (msgsnd(mainMsgQueueId, &endOfTimestepMsg, 0, 0) == -1) {
            perror("❌ Failed to notify validation of timestep completion");
        } else {
            printf("✅ Notified validation of timestep %d completion\n", timestep);
        }


        sleep(1);
    }

    return 0;
}


