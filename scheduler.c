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
#define MAX_AUTH_STRING_LEN 10
#define MAX_NEW_REQUESTS 100
#define MAX_CARGO_COUNT 10


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
    printf("🚢 Starting ship reading loop (print-only mode)...\n");

    while (1) {
        MessageStruct validationMsg;
        ssize_t msgSize = sizeof(MessageStruct) - sizeof(long);
        if (msgrcv(mainMsgQueueId, &validationMsg, msgSize, 1, 0) == -1) {
            perror("❌ Failed to receive message from validation");
            exit(EXIT_FAILURE);
        }

        int timestep = validationMsg.timestep;
        int numRequests = validationMsg.numShipRequests;  // now from the union


        printf("⏳ Timestep: %d | Ship Requests Received: %d\n", timestep, numRequests);

        // Exit condition
        if (numRequests == 0 && validationMsg.timestep == -1) {
            printf("🛑 Simulation end signal received.\n");
            break;
        }

        // Print all ship requests
        for (int i = 0; i < numRequests; i++) {
            ShipRequest req = sharedMemory->newShipRequests[i];
            printf("🚢 Ship ID: %d | Direction: %d | Category: %d | Max Wait: %d | Current Wait: %d | Auth: %s | Cargo: ", req.shipId, req.direction, req.category, req.waitingTime, req.waitingTime, sharedMemory->authStrings[req.shipId]);

            for (int j = 0; j < req.numCargo; j++) {
                printf("%d ", req.cargo[j]);
            }
            printf("\n");
        }

        // Notify end of timestep
        MessageStruct endMsg;
        endMsg.mtype = 5;
        if (msgsnd(mainMsgQueueId, &endMsg, 0, 0) == -1) {
            perror("❌ Failed to notify validation of timestep completion");
        } else {
            printf("✅ Timestep %d processing done\n", timestep);
        }

        sleep(1);  // Optional: simulate time delay
    }

    return 0;
}
