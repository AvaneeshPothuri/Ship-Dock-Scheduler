/*----------------------------------------------------------------------
  PORT‑MANAGEMENT SCHEDULER  –  Assignment‑2  (Operating Systems 2024‑25)
  --------------------------------------------------------------------*/
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <sys/ipc.h>
  #include <sys/msg.h>
  #include <sys/shm.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <errno.h>
  #include <time.h>
  
  /* -----------------------  CONSTANTS  --------------------------------*/
  #define MAX_DOCKS              30
  #define MAX_SOLVERS             8
  #define MAX_AUTH_STRING_LEN   100
  #define MAX_CRANES             25
  #define MAX_NEW_REQUESTS      100
  #define MAX_CARGO_COUNT       200
  #define MAX_TOTAL_SHIPS      1200
  
  /* -----------------------  DATA TYPES  -------------------------------*/
  typedef struct {
      long mtype;
      int  timestep, shipId, direction;
      int  dockId, cargoId, isFinished;
      union { int numShipRequests; int craneId; };
  } MessageStruct;
  
  typedef struct {
      int shipId, timestep, category, direction;
      int emergency, waitingTime;
      int numCargo;
      int cargo[MAX_CARGO_COUNT];
  } ShipRequest;
  
  typedef struct {
      char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
      ShipRequest newShipRequests[MAX_NEW_REQUESTS];
  } MainSharedMemory;
  
  typedef struct {
      int dockId;
      int numCranes;
      int craneCapacities[MAX_CRANES];
      int maxShipCategory;
  
      int isOccupied;
      int shipId, shipDirection;
      int cargoHandled, cargoTotal;
      int dockedAtTimestep, lastCargoMovedTimestep;
      int cargo[MAX_CARGO_COUNT];
  } DockStatus;
  
  /* solver IPC */
  typedef struct { long mtype; int dockId;
                   char authStringGuess[MAX_AUTH_STRING_LEN]; } SolverRequest;
  typedef struct { long mtype; int guessIsCorrect; } SolverResponse;
  
  /* -----------------------  HELPERS  ----------------------------------*/
  static int cmpShip(const void *a,const void *b)
  {
      const ShipRequest *A=a,*B=b;
      if (A->emergency!=B->emergency) return B->emergency-A->emergency;
      if (A->numCargo !=B->numCargo)  return A->numCargo -B->numCargo;
      return A->timestep - B->timestep;
  }
  
  static int findDock(DockStatus d[],int n,const ShipRequest *s)
  {
      for(int i=0;i<n;i++)
          if(!d[i].isOccupied && d[i].maxShipCategory>=s->category) return i;
      return -1;
  }
  
  static void sendDockMsg(const ShipRequest *s,int dockId,int ts,int mqid)
  {
      MessageStruct m={.mtype=2,.timestep=ts,.shipId=s->shipId,
                       .direction=s->direction,.dockId=dockId,.isFinished=0};
      msgsnd(mqid,&m,sizeof(MessageStruct)-sizeof(long),0);
  }
  
  /* comparators for qsort (need plain C functions) */
  static int cmpDockDesc(const void *p1,const void *p2,void *arg)
  {
      DockStatus *docks=(DockStatus*)arg;
      int a=*(const int*)p1, b=*(const int*)p2;
      return docks[b].maxShipCategory - docks[a].maxShipCategory;
  }
  static int cmpIntAsc(const void *p1,const void *p2)
  { return *(const int*)p1 - *(const int*)p2; }
  
  /* best‑fit emergency assignment */
  static void assignEmergencyShips(DockStatus docks[],int nDocks,
                                   ShipRequest pend[],int *pCnt,
                                   int ts,int mqid)
  {
      int dockIdx[MAX_DOCKS], dCnt=0;
      for(int i=0;i<nDocks;i++) if(!docks[i].isOccupied) dockIdx[dCnt++]=i;
  
      /* sort dock indices descending by capacity */
  #if defined(__GNUC__) && (__GNUC__*100+__GNUC_MINOR__)>=509
      qsort_r(dockIdx,dCnt,sizeof(int),cmpDockDesc,docks);
  #else
      /* portable fallback: bubble sort (dCnt ≤ 30) */
      for(int i=0;i<dCnt-1;i++)
          for(int j=i+1;j<dCnt;j++)
              if(docks[dockIdx[i]].maxShipCategory <
                 docks[dockIdx[j]].maxShipCategory){
                  int tmp=dockIdx[i]; dockIdx[i]=dockIdx[j]; dockIdx[j]=tmp;
              }
  #endif
  
      for(int di=0;di<dCnt;di++){
          int d=dockIdx[di], best=-1, bestCat=-1;
          for(int i=0;i<*pCnt;i++){
              ShipRequest *s=&pend[i];
              if(!(s->direction==1 && s->emergency)) continue;
              if(s->category> docks[d].maxShipCategory) continue;
              if(s->category>bestCat){ bestCat=s->category; best=i; }
          }
          if(best==-1) continue;
  
          ShipRequest *s=&pend[best];
          DockStatus  *dk=&docks[d];
  
          dk->isOccupied=1; dk->shipId=s->shipId; dk->shipDirection=1;
          dk->cargoHandled=0; dk->cargoTotal=s->numCargo;
          dk->dockedAtTimestep=ts; dk->lastCargoMovedTimestep=ts;
          for(int k=0;k<s->numCargo;k++) dk->cargo[k]=s->cargo[k];
  
          sendDockMsg(s,dk->dockId,ts,mqid);
  
          pend[best]=pend[--(*pCnt)];
      }
  }
  
  /* -----------------------  MAIN  -------------------------------------*/
  int main(int argc,char *argv[])
  {
      if(argc!=2){fprintf(stderr,"Usage: %s <testcase>\n",argv[0]);return 1;}
      srand((unsigned)time(NULL));
  
      char path[256]; snprintf(path,sizeof path,"testcase%s/input.txt",argv[1]);
      FILE *fp=fopen(path,"r"); if(!fp){perror("input.txt");return 1;}
  
      key_t shmKey,mainMQKey,solverKey[MAX_SOLVERS];
      int numSolvers,numDocks;
      fscanf(fp,"%d %d %d",&shmKey,&mainMQKey,&numSolvers);
      for(int i=0;i<numSolvers;i++) fscanf(fp,"%d",&solverKey[i]);
      fscanf(fp,"%d",&numDocks); fgetc(fp);
  
      DockStatus docks[MAX_DOCKS]={0};
      for(int i=0;i<numDocks;i++){
          docks[i].dockId=i;
          char line[256]; fgets(line,sizeof line,fp);
          char *tok=strtok(line," \t\n"); int idx=0;
          while(tok){
              int v=atoi(tok);
              if(idx==0) docks[i].maxShipCategory=v;
              else if(idx-1<MAX_CRANES) docks[i].craneCapacities[idx-1]=v;
              idx++; tok=strtok(NULL," \t\n");
          }
          docks[i].numCranes=idx-1;
      }
      fclose(fp);
  
      int shmId=shmget(shmKey,sizeof(MainSharedMemory),0666);
      if(shmId==-1){perror("shmget");return 1;}
      MainSharedMemory *shm=shmat(shmId,NULL,0);
      if(shm==(void*)-1){perror("shmat");return 1;}
  
      int mainMQ=msgget(mainMQKey,0666);
      if(mainMQ==-1){perror("msgget main");return 1;}
  
      int solverMQ[MAX_SOLVERS];
      for(int i=0;i<numSolvers;i++){
          solverMQ[i]=msgget(solverKey[i],0666);
          if(solverMQ[i]==-1){fprintf(stderr,"solver mq %d\n",i);return 1;}
      }
  
      ShipRequest pending[MAX_TOTAL_SHIPS]; int pCnt=0;
      static const char charset[]={'5','6','7','8','9','.'};
  
      for(;;){
          MessageStruct vmsg;
          if(msgrcv(mainMQ,&vmsg,sizeof vmsg-sizeof(long),1,0)==-1){
              if(errno==EIDRM) break; perror("msgrcv"); return 1;
          }
          int ts=vmsg.timestep, newReq=vmsg.numShipRequests;
          if(newReq==0 && ts==-1) break;
  
          for(int i=0;i<newReq;i++) pending[pCnt++]=shm->newShipRequests[i];
  
          for(int i=0;i<pCnt;){
              ShipRequest *s=&pending[i];
              if(s->direction==1 && !s->emergency &&
                 ts > s->timestep + s->waitingTime){
                  pending[i]=pending[--pCnt];
              }else ++i;
          }
  
          assignEmergencyShips(docks,numDocks,pending,&pCnt,ts,mainMQ);
  
          qsort(pending,pCnt,sizeof(ShipRequest),cmpShip);
          for(int i=0;i<pCnt;){
              int d=findDock(docks,numDocks,&pending[i]);
              if(d==-1){ ++i; continue; }
              DockStatus *dk=&docks[d]; ShipRequest *s=&pending[i];
  
              dk->isOccupied=1; dk->shipId=s->shipId; dk->shipDirection=s->direction;
              dk->cargoHandled=0; dk->cargoTotal=s->numCargo;
              dk->dockedAtTimestep=ts; dk->lastCargoMovedTimestep=ts;
              for(int k=0;k<s->numCargo;k++) dk->cargo[k]=s->cargo[k];
  
              sendDockMsg(s,d,ts,mainMQ);
              pending[i]=pending[--pCnt];
          }
  
          for(int i=0;i<numDocks;i++){
              DockStatus *d=&docks[i];
              if(!d->isOccupied) continue;
              if(d->dockedAtTimestep==ts) continue;
              if(d->cargoHandled>=d->cargoTotal) continue;
  
              for(int c=0;c<d->numCranes && d->cargoHandled<d->cargoTotal;c++){
                  int w=d->cargo[d->cargoHandled];
                  if(d->craneCapacities[c]<w) continue;
  
                  MessageStruct m={.mtype=4,.timestep=ts,.shipId=d->shipId,
                                   .direction=d->shipDirection,.dockId=d->dockId,
                                   .cargoId=d->cargoHandled,.craneId=c};
                  msgsnd(mainMQ,&m,sizeof m-sizeof(long),0);
  
                  d->cargoHandled++; d->lastCargoMovedTimestep=ts;
              }
          }
  
          for(int i=0;i<numDocks;i++){
              DockStatus *d=&docks[i];
              if(!d->isOccupied) continue;
              if(d->cargoHandled<d->cargoTotal) continue;
              if(d->lastCargoMovedTimestep==ts) continue;
  
              int len=d->lastCargoMovedTimestep - d->dockedAtTimestep;
              if(len<=0 || len>=MAX_AUTH_STRING_LEN) continue;
  
              for(int s=0;s<numSolvers;s++){
                  SolverRequest set={.mtype=1,.dockId=d->dockId};
                  msgsnd(solverMQ[s],&set,sizeof set-sizeof(long),0);
              }
  
              char guess[MAX_AUTH_STRING_LEN]; guess[len]='\0';
              int solved=0;
              for(long att=0;att<400000 && !solved;att++){
                  for(int j=0;j<len;j++) guess[j]=charset[rand()%6];
                  if(guess[0]=='.'||guess[len-1]=='.') continue;
  
                  int sid=att%numSolvers;
                  SolverRequest g={.mtype=2,.dockId=d->dockId};
                  strncpy(g.authStringGuess,guess,len+1);
                  msgsnd(solverMQ[sid],&g,sizeof g-sizeof(long),0);
  
                  SolverResponse r;
                  if(msgrcv(solverMQ[sid],&r,sizeof r-sizeof(long),3,0)==-1) continue;
                  if(r.guessIsCorrect==1){
                      solved=1;
                      strncpy(shm->authStrings[d->dockId],guess,MAX_AUTH_STRING_LEN-1);
  
                      MessageStruct u={.mtype=3,.timestep=ts,.shipId=d->shipId,
                                       .direction=d->shipDirection,.dockId=d->dockId,
                                       .isFinished=1};
                      msgsnd(mainMQ,&u,sizeof u-sizeof(long),0);
                      d->isOccupied=0; d->shipId=-1;
                  }
              }
          }
  
          MessageStruct done={.mtype=5};
          msgsnd(mainMQ,&done,0,0);
      }
      return 0;
  }
  
