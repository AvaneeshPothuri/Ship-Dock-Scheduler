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

    printf("📨 Main Message Queue Key: %d\n", mainMsgQueueKey);
    printf("🧠 Number of Solver Queues: %d\n", numSolvers);

    for (int i = 0; i < numSolvers; i++) {
        fscanf(file, "%d", &solverQueueKeys[i]);
    }
    printf("🧩 Solver Message Queue Keys: ");
    for (int i = 0; i < numSolvers; i++) {
        printf("%d ", solverQueueKeys[i]);
    }
    printf("\n");

    fscanf(file, "%d", &numDocks);
    printf("🚢 Number of Docks: %d\n", numDocks);

    int ch;
    while ((ch = fgetc(file)) != '\n' && ch != EOF);

    DockStatus dockStatuses[MAX_DOCKS];

    for (int i = 0; i < numDocks; i++) {
        dockStatuses[i].dockId = i + 1;
        dockStatuses[i].isOccupied = 0;
        dockStatuses[i].shipId = -1;
        dockStatuses[i].shipDirection = -1;
        dockStatuses[i].cargoHandled = 0;
        dockStatuses[i].cargoTotal = 0;

        char line[256];
        if (fgets(line, sizeof(line), file) == NULL) {
            fprintf(stderr, "Error reading dock line %d\n", i + 1);
            exit(1);
        }

        char* token = strtok(line, " \t\n");
        int count = 0;
        while (token != NULL) {
            int val = atoi(token);
            if (count == 0) {
                dockStatuses[i].maxShipCategory = val;  // First number is category
            } else {
                dockStatuses[i].craneCapacities[count - 1] = val;
            }
            count++;
            token = strtok(NULL, " \t\n");
        }
        dockStatuses[i].numCranes = count - 1;  // Exclude the category
    }

    printf("🛠️  Dock Details:\n");
    for (int i = 0; i < numDocks; i++) {
        printf("    - Dock ID: %d | Category: %d | Cranes (%d): ",
               dockStatuses[i].dockId,
               dockStatuses[i].maxShipCategory,
               dockStatuses[i].numCranes);
        for (int j = 0; j < dockStatuses[i].numCranes; j++) {
            printf("%d ", dockStatuses[i].craneCapacities[j]);
        }
        printf("\n");
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

    initializeDockStatuses(dockStatuses, docks, numDocks);

    printf("✔️ IPC mechanisms set up successfully.\n");

    int timestep = 0;
    printf("🚢 Starting scheduling loop...\n");

    ShipRequest waitingShips[100];
    int waitingCount = 0;
    
    int lastTimestep = -1;
    timestep = sharedMemory->timestep;

    while (1) {
        printf("⏳ Timestep: %d | Num Requests: %d\n", timestep, sharedMemory->numShipRequests);
        int numRequests = sharedMemory->numShipRequests;

        if (numRequests == 0 && timestep > 0) {
            printf("⏹️  No more ship requests. Terminating.\n");
            break;
        }

        // Add new arrivals to waitingShips
        for (int i = 0; i < numRequests; i++) {
            waitingShips[waitingCount++] = sharedMemory->newShipRequests[i];
        }

        // Update waiting times
        for (int i = 0; i < waitingCount; i++) {
            waitingShips[i].currentWaitingTime++;
        }

        // Remove expired ships
        int newWaitingCount = 0;
        for (int i = 0; i < waitingCount; i++) {
            if (waitingShips[i].currentWaitingTime <= waitingShips[i].maxWaitingTime) {
                waitingShips[newWaitingCount++] = waitingShips[i];
            } else {
                printf("❌ Ship %d expired and was removed\n", waitingShips[i].shipId);
            }
        }
        waitingCount = newWaitingCount;

        // Sort waitingShips based on FCFS and priority logic
        for (int i = 0; i < waitingCount - 1; i++) {
            for (int j = i + 1; j < waitingCount; j++) {
                if ((waitingShips[i].shipCategory > waitingShips[j].shipCategory) ||
                    (waitingShips[i].shipCategory == waitingShips[j].shipCategory &&
                     waitingShips[i].currentWaitingTime < waitingShips[j].currentWaitingTime)) {
                    ShipRequest temp = waitingShips[i];
                    waitingShips[i] = waitingShips[j];
                    waitingShips[j] = temp;
                }
            }
        }

        // Assign ships to docks
        for (int i = 0; i < waitingCount; i++) {
            ShipRequest* ship = &waitingShips[i];
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
                    msg.craneId = -1;
                    msg.cargoId = -1;
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

        MessageStruct endOfTimestepMsg;
        endOfTimestepMsg.mtype = 5;

        if (msgsnd(mainMsgQueueId, &endOfTimestepMsg, 0, 0) == -1) {
            perror("❌ Failed to notify validation of timestep completion");
        } else {
            printf("✅ Notified validation of timestep %d completion\n", timestep);
            timestep++;
        }

        while (sharedMemory->timestep == lastTimestep) {
            usleep(1000);  // Sleep for 1ms
        }
        
        sleep(1);
    }

    return 0;
}
