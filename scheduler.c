#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define MAX_AUTH_STRING_LEN 100
#define MAX_SOLVERS 10
#define MAX_DOCKS 100
#define MAX_CARGO 100
#define MAX_SHIPS 100

typedef struct {
    long mtype;
    int dockId;
    int shipId;
    int direction;
    int cargoId;
    int craneId;
    int timestep;
    int isFinished;
    int numShipRequests;
} MessageStruct;

typedef struct {
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

typedef struct {
    int shipId;
    int category;
    int direction;
    int numCargoItems;
    int cargoIds[MAX_CARGO];
    int cargoWeights[MAX_CARGO];
    int waitingTime;
} ShipRequest;

typedef struct {
    ShipRequest newShipRequests[MAX_SHIPS];
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
} SharedMemory;

int shmId;
SharedMemory* shmPtr = NULL;

int mainMsgqId;
int solverMsgqIds[MAX_SOLVERS];
int numSolvers;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int testcaseNum = atoi(argv[1]);
    char inputFilePath[100];
    snprintf(inputFilePath, sizeof(inputFilePath), "testcase%d/input.txt", testcaseNum);

    FILE *fp = fopen(inputFilePath, "r");
    if (!fp) {
        perror("Error opening input.txt");
        exit(EXIT_FAILURE);
    }

    key_t shmKey, mainMsgqKey, solverMsgqKeys[MAX_SOLVERS];

    if (fscanf(fp, "%d", &shmKey) != 1 ||
        fscanf(fp, "%d", &mainMsgqKey) != 1 ||
        fscanf(fp, "%d", &numSolvers) != 1) {
        fprintf(stderr, "Error reading keys from %s\n", inputFilePath);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    if (numSolvers > MAX_SOLVERS) {
        fprintf(stderr, "Too many solvers: %d (max is %d)\n", numSolvers, MAX_SOLVERS);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < numSolvers; i++) {
        if (fscanf(fp, "%d", &solverMsgqKeys[i]) != 1) {
            fprintf(stderr, "Error reading solver message queue key %d\n", i);
            fclose(fp);
            exit(EXIT_FAILURE);
        }
    }

    fclose(fp);

    shmId = shmget(shmKey, 0, 0666);
    if (shmId == -1) {
        perror("shmget failed (check if validation.out is running)");
        exit(1);
    } else {
        printf("Got shmid = %d\n", shmId);
    }

    shmPtr = (SharedMemory*) shmat(shmId, NULL, 0);
    if (shmPtr == (void*) -1) {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }

    printf("✅ Attached to shared memory (shmId: %d)\n", shmId);

    mainMsgqId = msgget(mainMsgqKey, 0666);
    if (mainMsgqId == -1) {
        perror("msgget for validation queue failed");
        exit(EXIT_FAILURE);
    }

    printf("✅ Connected to main message queue (id: %d)\n", mainMsgqId);

    for (int i = 0; i < numSolvers; i++) {
        solverMsgqIds[i] = msgget(solverMsgqKeys[i], 0666);
        if (solverMsgqIds[i] == -1) {
            fprintf(stderr, "msgget failed for solver %d (key: %d): %s\n", i, solverMsgqKeys[i], strerror(errno));
            exit(EXIT_FAILURE);
        }
        printf("✅ Connected to solver %d message queue (id: %d)\n", i, solverMsgqIds[i]);
    }

    printf("\n🎉 IPC Setup Complete! Ready to proceed.\n");

    return 0;
}

