#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#define MAX_DOCKS 30
#define MAX_SOLVERS 8
#define MAX_AUTH_STRING_LEN 100
#define MAX_NEW_REQUESTS 100
#define MAX_CARGO_COUNT 200
#define MAX_QUEUE_SIZE 200

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
    int dockedAtTimestep;
    int cargo[MAX_CARGO_COUNT];
    int numCargo;
    int lastCargoMovedTimestep;
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

typedef struct {
    ShipRequest* ships[MAX_QUEUE_SIZE];
    int front, rear;
} ShipQueue;

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

void assignShipToDock(DockStatus* dock, ShipRequest* ship, int currentTimestep) {
    dock->isOccupied = 1;
    dock->shipId = ship->shipId;
    dock->shipDirection = ship->direction;
    dock->cargoHandled = 0;
    dock->cargoTotal = ship->numCargo;
    dock->dockedAtTimestep = currentTimestep;
    dock->numCargo = ship->numCargo;
    
    for (int i = 0; i < ship->numCargo; i++) {
        dock->cargo[i] = ship->cargo[i];
    }
}

int trySolveAuth(int solverQueueId, int dockId, const char* authString) {
    SolverRequest req;
    req.mtype = 1;
    req.dockId = dockId;
    strncpy(req.authStringGuess, authString, sizeof(req.authStringGuess));

    if (msgsnd(solverQueueId, &req, sizeof(SolverRequest) - sizeof(long), 0) == -1) {
        perror("❌ Failed to send auth guess to solver");
        return 0;
    }

    SolverResponse resp;
    if (msgrcv(solverQueueId, &resp, sizeof(SolverResponse) - sizeof(long), 2, 0) == -1) {
        perror("❌ Failed to receive solver response");
        return 0;
    }

    return resp.guessIsCorrect;
}

void handleUndocking(DockStatus docks[], int numDocks, MainSharedMemory* sharedMemory,
                     int solverQueueIds[], int numSolvers, int mainMsgQueueId, int currentTimestep) {
    for (int i = 0; i < numDocks; i++) {
        if (!docks[i].isOccupied) continue;
        if (docks[i].cargoHandled < docks[i].cargoTotal) continue;
        if (docks[i].lastCargoMovedTimestep == currentTimestep) continue;

        int dockId = docks[i].dockId;
        int shipId = docks[i].shipId;
        int dockedTime = docks[i].dockedAtTimestep;
        int duration = currentTimestep - dockedTime;

        if (duration <= 0) continue;

        printf("🚪 Initiating undocking for Ship %d at Dock %d...\n", shipId, dockId);

        for (int s = 0; s < numSolvers; s++) {
            SolverRequest setupMsg = { .mtype = 1, .dockId = dockId };
            memset(setupMsg.authStringGuess, 0, sizeof(setupMsg.authStringGuess));
            msgsnd(solverQueueIds[s], &setupMsg, sizeof(SolverRequest) - sizeof(long), 0);
        }

        char charset[] = {'5', '6', '7', '8', '9', '.'};
        char guess[duration + 1];
        guess[duration] = '\0';

        int solved = 0;

        for (int s = 0; s < numSolvers && !solved; s++) {
            for (int attempt = 0; attempt < 1000000 && !solved; attempt++) {
                for (int j = 0; j < duration; j++) {
                    guess[j] = charset[rand() % 6];
                }

                if (guess[0] == '.' || guess[duration - 1] == '.') continue;

                SolverRequest guessMsg = { .mtype = 2, .dockId = dockId };
                strncpy(guessMsg.authStringGuess, guess, sizeof(guessMsg.authStringGuess) - 1);
                guessMsg.authStringGuess[sizeof(guessMsg.authStringGuess) - 1] = '\0';

                msgsnd(solverQueueIds[s], &guessMsg, sizeof(SolverRequest) - sizeof(long), 0);

                SolverResponse response;
                if (msgrcv(solverQueueIds[s], &response, sizeof(SolverResponse) - sizeof(long), 3, 0) != -1) {
                    if (response.guessIsCorrect == 1) {
                        solved = 1;
                        strncpy(sharedMemory->authStrings[dockId], guess, MAX_AUTH_STRING_LEN - 1);
                        sharedMemory->authStrings[dockId][MAX_AUTH_STRING_LEN - 1] = '\0';

                        printf("✅ Correct auth string \"%s\" found by Solver %d for Dock %d\n", guess, s, dockId);

                        MessageStruct undockMsg = {
                            .mtype = 3,
                            .timestep = currentTimestep,
                            .shipId = shipId,
                            .dockId = dockId,
                            .direction = docks[i].shipDirection,
                            .isFinished = 1
                        };

                        if (msgsnd(mainMsgQueueId, &undockMsg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                            perror("❌ Failed to send undocking message to validation");
                        } else {
                            printf("📤 Undocking message sent for Ship %d at Dock %d\n", shipId, dockId);
                        }

                        docks[i].isOccupied = 0;
                        docks[i].shipId = -1;
                        docks[i].shipDirection = -1;
                        docks[i].cargoHandled = 0;
                        docks[i].cargoTotal = 0;
                    }
                }
            }
        }

        if (!solved) {
            printf("❌ Failed to solve auth for Dock %d. Keeping dock occupied.\n", dockId);
        }
    }
}

void initQueue(ShipQueue* q) {
    q->front = q->rear = 0;
}

int isQueueEmpty(ShipQueue* q) {
    return q->front == q->rear;
}

void enqueue(ShipQueue* q, ShipRequest* ship) {
    if ((q->rear + 1) % MAX_QUEUE_SIZE == q->front) {
        printf("❗ Queue full. Cannot enqueue ship %d\n", ship->shipId);
        return;
    }
    q->ships[q->rear] = ship;
    q->rear = (q->rear + 1) % MAX_QUEUE_SIZE;
}

ShipRequest* dequeue(ShipQueue* q) {
    if (isQueueEmpty(q)) return NULL;
    ShipRequest* ship = q->ships[q->front];
    q->front = (q->front + 1) % MAX_QUEUE_SIZE;
    return ship;
}

void* handleShipThread(void* arg) {
    ShipRequest* ship = (ShipRequest*)arg;
    printf("🚀 Thread started for Ship %d (Emergency: %d)\n", ship->shipId, ship->emergency);
    pthread_exit(NULL);
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
        dockStatuses[i].dockId = i;
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
            printf("     📦 Cargo Weights: ");
            for (int c = 0; c < req->numCargo; c++) {
                printf("%d ", req->cargo[c]);
            }
            printf("\n");

            int dockIdx = findSuitableDock(dockStatuses, numDocks, req);
            if (dockIdx == -1) {
                printf("     ❌ No suitable dock available.\n");
                continue;
            }

            assignShipToDock(&dockStatuses[dockIdx], &incomingRequests[i], timestep);
            MessageStruct response;
            response.mtype = 2;
            response.timestep = timestep;
            response.shipId = req->shipId;
            response.dockId = dockStatuses[dockIdx].dockId;
            response.direction = req->direction;
            response.isFinished = 0;

            if (msgsnd(mainMsgQueueId, &response, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("❌ Failed to send docking confirmation to validation");
            }

            printf("     ✅ Assigned to Dock %d (Max Category %d)\n", 
                   dockStatuses[dockIdx].dockId, dockStatuses[dockIdx].maxShipCategory);
        }

        for (int i = 0; i < numDocks; i++) {
            if (!dockStatuses[i].isOccupied) continue;
            if (dockStatuses[i].dockedAtTimestep == timestep) continue;

            int shipId = dockStatuses[i].shipId;
            int direction = dockStatuses[i].shipDirection;
            int cargoHandled = dockStatuses[i].cargoHandled;
            int cargoTotal = dockStatuses[i].cargoTotal;

            if (cargoHandled >= cargoTotal) continue;

            ShipRequest* ship = NULL;
            for (int j = 0; j < MAX_NEW_REQUESTS; j++) {
                if (sharedMemory->newShipRequests[j].shipId == shipId) {
                    ship = &sharedMemory->newShipRequests[j];
                    break;
                }
            }

            if (!ship) continue;

            for (int c = 0; c < dockStatuses[i].numCranes; c++) {
                if (cargoHandled >= cargoTotal) break;

                int cargoWeight = dockStatuses[i].cargo[cargoHandled];

                if (dockStatuses[i].craneCapacities[c] >= cargoWeight) {
                    MessageStruct msg;
                    msg.mtype = 4;
                    msg.timestep = timestep;
                    msg.shipId = shipId;
                    msg.direction = direction;
                    msg.dockId = dockStatuses[i].dockId;
                    msg.cargoId = cargoHandled;
                    msg.craneId = c;
                    msg.isFinished = 0;

                    if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                        perror("❌ Failed to send cargo handling message");
                    } else {
                        dockStatuses[i].cargoHandled++;
                        dockStatuses[i].lastCargoMovedTimestep = timestep;
                        printf("⚙️  Crane %d moved cargo %d (Weight: %d) at Dock %d for Ship %d\n", 
                               c, cargoHandled, cargoWeight, dockStatuses[i].dockId, shipId);
                        cargoHandled = dockStatuses[i].cargoHandled;
                    }
                }
            }
        }

        handleUndocking(dockStatuses, numDocks, sharedMemory, solverQueueIds, numSolvers, mainMsgQueueId, timestep);

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

