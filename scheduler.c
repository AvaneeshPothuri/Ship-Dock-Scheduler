#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SHIPS 100
#define MAX_DOCKS 10
#define MAX_SOLVERS 10

typedef struct {
    int ship_id;
    int arrival_time;
    int burst_time;
    int type; // 0 = emergency, 1 = normal
    int start_time;
    int completion_time;
    int turnaround_time;
    int waiting_time;
    int response_time;
    int is_completed;
} Ship;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_folder_name>\n", argv[0]);
        return 1;
    }

    char input_path[256];
    snprintf(input_path, sizeof(input_path), "testcase%s/input.txt", argv[1]);

    FILE *file = fopen(input_path, "r");
    if (!file) {
        perror("Error opening input.txt");
        return 1;
    }

    int shm_key, main_msg_key;
    int m, solver_msg_queues[MAX_SOLVERS];
    int n, dock_categories[MAX_DOCKS], crane_caps[MAX_DOCKS][10];

    fscanf(file, "%d", &shm_key);
    fscanf(file, "%d", &main_msg_key);
    fscanf(file, "%d", &m);

    for (int i = 0; i < m; i++) {
        fscanf(file, "%d", &solver_msg_queues[i]);
    }

    fscanf(file, "%d", &n);
    for (int i = 0; i < n; i++) {
        fscanf(file, "%d", &dock_categories[i]);
        for (int j = 0; j < dock_categories[i]; j++) {
            fscanf(file, "%d", &crane_caps[i][j]);
        }
    }

    fclose(file);

    printf("Configuration Loaded from %s:\n", input_path);
    printf("Shared Memory Key: %d\n", shm_key);
    printf("Main Message Queue Key: %d\n", main_msg_key);
    printf("Number of Solvers: %d\n", m);
    for (int i = 0; i < m; i++) {
        printf("Solver %d MQ Key: %d\n", i + 1, solver_msg_queues[i]);
    }
    printf("Number of Docks: %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("Dock %d Category: %d -> Cranes: ", i + 1, dock_categories[i]);
        for (int j = 0; j < dock_categories[i]; j++) {
            printf("%d ", crane_caps[i][j]);
        }
        printf("\n");
    }

    Ship ships[MAX_SHIPS];
    int total_ships = 0;
    while (scanf("%d %d %d %d", &ships[total_ships].ship_id,
                                   &ships[total_ships].arrival_time,
                                   &ships[total_ships].burst_time,
                                   &ships[total_ships].type) == 4) {
        ships[total_ships].is_completed = 0;
        printf("Received ship: ID=%d, AT=%d, BT=%d, Type=%s\n",
               ships[total_ships].ship_id,
               ships[total_ships].arrival_time,
               ships[total_ships].burst_time,
               ships[total_ships].type == 0 ? "Emergency" : "Normal");
        total_ships++;
    }

    printf("Total ships received: %d\n", total_ships);

    int current_time = 0, completed = 0;

    while (completed < total_ships) {
        int idx = -1;
        int min_bt = 1e9;

        for (int i = 0; i < total_ships; i++) {
            if (!ships[i].is_completed && ships[i].arrival_time <= current_time) {
                if (ships[i].type == 0 && ships[i].burst_time < min_bt) {
                    min_bt = ships[i].burst_time;
                    idx = i;
                }
            }
        }

        if (idx == -1) {
            for (int i = 0; i < total_ships; i++) {
                if (!ships[i].is_completed && ships[i].arrival_time <= current_time) {
                    if (ships[i].type == 1 && ships[i].burst_time < min_bt) {
                        min_bt = ships[i].burst_time;
                        idx = i;
                    }
                }
            }
        }

        if (idx == -1) {
            current_time++;
            continue;
        }

        ships[idx].start_time = current_time;
        ships[idx].completion_time = current_time + ships[idx].burst_time;
        ships[idx].turnaround_time = ships[idx].completion_time - ships[idx].arrival_time;
        ships[idx].waiting_time = ships[idx].turnaround_time - ships[idx].burst_time;
        ships[idx].response_time = ships[idx].start_time - ships[idx].arrival_time;
        ships[idx].is_completed = 1;

        current_time = ships[idx].completion_time;
        completed++;
    }

    printf("ShipID\tType\tAT\tBT\tCT\tTAT\tWT\tRT\n");
    for (int i = 0; i < total_ships; i++) {
        printf("%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n",
               ships[i].ship_id,
               ships[i].type == 0 ? "E" : "N",
               ships[i].arrival_time,
               ships[i].burst_time,
               ships[i].completion_time,
               ships[i].turnaround_time,
               ships[i].waiting_time,
               ships[i].response_time);
    }

    return 0;
}

