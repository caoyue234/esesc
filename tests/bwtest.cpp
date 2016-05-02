//cache test
//
// TODO: 
//
// -no double snoop detect
//
// -Create represure in the L1 cache with requests.
//
// -3 cores doing a write simultaneously, how to handle snoops

#include <stdio.h>
#include <exception>
#include <strings.h>

#include "CCache.h"
#include "GProcessor.h"
#include "MemObj.h"
#include "MemRequest.h"
#include "callback.h"
#include "MemorySystem.h"
#include "SescConf.h"
#include "DInst.h"
#include "nanassert.h"
#include "RAWDInst.h"
#include "Instruction.h"
#include "MemStruct.h"


#ifdef DEBUG_CALLPATH
extern bool forcemsgdump;
#endif

static int rd_pending = 0;
static int wr_pending = 0;
static int num_operations=0;

#ifdef ENABLE_NBSD
#include "MemRequest.h"
void meminterface_start_snoop_req(uint64_t addr, bool inv, uint16_t coreid, bool dcache, void *_mreq) {
  I(0);
}
void meminterface_req_done(void *param, int mesi) {
  I(0);
}
#endif

enum currState{
  Modified, //0
  Exclusive, //1
  Shared,//2
  Invalid //3
};

currState getState(CCache *cache, AddrType addr){
  currState state;

  if(cache->Modified(addr))
    state=Modified;

  if(cache->Exclusive(addr))
    state=Exclusive;
  
  if(cache->Shared(addr))
    state=Shared;

  if(cache->Invalid(addr))
      state=Invalid;

  return state;
}

DInst *ld;
DInst *st;
RAWDInst rinst;

bool one_finished;

void rdDone(DInst *dinst) {

  printf("rddone @%lld\n",(long long)globalClock);

  one_finished = true;

	rd_pending--;
  dinst->recycle();
}

void wrDone(DInst *dinst) {

  printf("wrdone @%lld\n",(long long)globalClock);

  one_finished = true;
	wr_pending--;
  dinst->recycle();
}

static void waitAllMemOpsDone() {
  while(rd_pending || wr_pending)
    EventScheduler::advanceClock();
}

typedef CallbackFunction1<DInst *, &rdDone> rdDoneCB;
typedef CallbackFunction1<DInst *, &wrDone> wrDoneCB;

static void doread(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *ldClone = ld->clone();
  ldClone->setAddr(addr);

  while(cache->isBusy(addr))
    EventScheduler::advanceClock();

  rdDoneCB *cb = rdDoneCB::create(ldClone);
  printf("rd %x @%lld\n", (unsigned int)addr,(long long)globalClock);

  MemRequest::sendReqRead(cache, ldClone->getStatsFlag(), addr, cb);
  rd_pending++;
}

static void doprefetch(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *ldClone = ld->clone();
  ldClone->setAddr(addr);

  while(cache->isBusy(addr))
    EventScheduler::advanceClock();

  rdDoneCB *cb = rdDoneCB::create(ldClone);
  printf("rd %x @%lld\n", (unsigned int)addr,(long long)globalClock);

  MemRequest::sendReqReadPrefetch(cache, ldClone->getStatsFlag(), addr, cb);
  rd_pending++;
}

static void dowrite(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *stClone = st->clone();
  stClone->setAddr(addr);

	while(cache->isBusy(addr))
		EventScheduler::advanceClock();

  wrDoneCB *cb = wrDoneCB::create(stClone);
  printf("wr %x @%lld\n", (unsigned int)addr,(long long)globalClock);

	MemRequest::sendReqWrite(cache, stClone->getStatsFlag(), addr, cb);
	wr_pending++;
}

static void dodisp(MemObj *cache, AddrType addr)
{
  num_operations++;
  DInst *stClone = st->clone();
  stClone->setAddr(addr);

	while(cache->isBusy(addr))
		EventScheduler::advanceClock();

  wrDoneCB *cb = wrDoneCB::create(stClone);
  printf("disp %x @%lld\n", (unsigned int)addr,(long long)globalClock);

  MRouter *router= cache->getRouter();
  MemObj *mobj=router->getDownNode();
	MemRequest::sendDirtyDisp(mobj, cache, addr, stClone->getStatsFlag(), cb);
	wr_pending++;
}


bool pluggedin = false;
GMemorySystem *gms_p0 = 0;
GMemorySystem *gms_p1 = 0;
GMemorySystem *gms_p4 = 0;
void initialize(){
  
  if(!pluggedin){
    int arg1          = 1;
    const char *arg2[] = {"cachetest", 0};
    //setenv("ESESC_tradCORE_DL1","DL1_core DL1",1);

    SescConf = new SConfig(arg1, arg2);
    //unsetenv("ESESC_tradCore_DL1");
    gms_p0 = new MemorySystem(0);
    gms_p0->buildMemorySystem();
    gms_p1 = new MemorySystem(1);
    gms_p1->buildMemorySystem();
    gms_p4 = new MemorySystem(4);
    gms_p4->buildMemorySystem();
    pluggedin=true;
#ifdef DEBUG_CALLPATH
    forcemsgdump = true;
#endif
  }

  // Create a LD (e5d33000) with PC = 0xfeeffeef and address 1203
  Instruction *ld_inst = new Instruction();
  ld_inst->set(iLALU_LD, LREG_R1, LREG_R2, LREG_R3, LREG_R4);
  ld = DInst::create(ld_inst, 0xfeeffeef, 1203, 0, true);

  Instruction *st_inst = new Instruction();
  st_inst->set(iSALU_ST, LREG_R1, LREG_R2, LREG_R3, LREG_R4);
  st = DInst::create(st_inst, 0x410, 0x400, 0, true);
}


CCache *getDL1(GMemorySystem *gms){
  MemObj *dl1=gms->getDL1();
  if (strcasecmp(dl1->getDeviceType(),"cache")!=0) {
    printf("going down from %s\n",dl1->getDeviceType());
    MRouter *router= dl1->getRouter();
    dl1=router->getDownNode();
    if (strcasecmp(dl1->getDeviceType(),"cache")!=0) {
      printf("ERROR: Neither first or second level is a cache %s\n",dl1->getDeviceType());
      exit(-1);
    }
  }

  CCache *cdl1=static_cast<CCache*>(dl1);
  return cdl1;
}

CCache *getL3(MemObj *L2) {
	MRouter *router2= L2->getRouter();
  MemObj *L3=router2->getDownNode();
  if (strncasecmp(L3->getName(),"L3",2)!=0)
    return 0;
  if (strcasecmp(L3->getDeviceType(),"cache")!=0)
    return 0;

	CCache *l3c= static_cast<CCache*>(L3);
  return l3c;
}

CCache *getL2(MemObj *P0DL1) {
	MRouter *router=P0DL1->getRouter();
	MemObj *L2=router->getDownNode();
	 CCache *l2c=static_cast<CCache*>(L2);
	//l2c->setNeedsCoherence();
   return l2c;
}


class MemInterface {
protected:
  CCache *p0dl1;
  CCache *p1dl1;
  CCache *p4dl1;

  CCache *p0l2;
  CCache *p1l2;
  CCache *p4l2;

  CCache *l3;
public:
  MemInterface() {
    initialize();
    p0dl1=getDL1(gms_p0);

    p0l2 =getL2(p0dl1);
    p0l2->setCoreDL1(0); // to allow fake L2 direct accesses

    p1dl1=getDL1(gms_p1);
    p1l2 =getL2(p1dl1);

    p4dl1=getDL1(gms_p4);
    p4l2 =getL2(p4dl1);

    l3   =getL3(p0l2);
  }

  void test1(bool rd, int id, int wait);
};


void MemInterface::test1(bool rd, int id, int wait) {

  CCache *cache = 0;

  if (id==0 || id==2)
    cache = p0l2;
  else if (id==1)
    cache = p1l2;
  else if (id==4)
    cache = p4l2;

  if (cache == 0) {
    fprintf(stderr,"ERROR: Invalid cache source selection %d\n",id);
    exit(-1);
  }

  printf("BEGIN L2 BW test\n");

  Time_t start;
  int conta;
  double t;

  start = globalClock;
  conta = 0;
  one_finished = false;

  int max_reqs_cycle      = 4;

  Time_t last_globalClock = globalClock;
  for(int i=0;i<32768;i+=64) {
    if (rd) {
      if (id == 2) {
        doread(p0l2,0x1000 + i);
        doread(p4l2,0x10001000 + i);
        conta++;
        conta++;
      }else{
        conta++;
        doread(cache,0x1000 + i);
      }
    } else {
      conta++;
      dodisp(cache,0x1000 + i);
    }
    for(int j=0;j<wait;j++)
      EventScheduler::advanceClock();

    if (last_globalClock == globalClock) {
      max_reqs_cycle--;
      if (max_reqs_cycle<0) {
        max_reqs_cycle = 4;
        EventScheduler::advanceClock();
      }
    }
    last_globalClock = globalClock;

    if (!one_finished) {
      start = globalClock;
      printf("HERE %lld\n", start);
    }
  }
  waitAllMemOpsDone();
  Time_t l2filltime = globalClock - start;
  t = ((double)l2filltime)/conta;
  double mb = 64.0/t;

  printf("L2 BW miss test wait=%d %lld (%g cycles/l2_miss) %g GB/s\n", wait, l2filltime, t,mb);
}

int main(int argc, char **argv) {

  MemInterface mi;

  if (argc!=5) {
    fprintf(stderr,"Usage:\n\t%s <L2|L1> <RD|DISP|INV> coreid wait\n",argv[0]);
    exit(-3);
  }

  int coreid = atoi(argv[3]);
  int wait   = atoi(argv[4]);

  if (wait<0 || wait>64) {
    fprintf(stderr,"ERROR: Wait out of linits [0..64] not %d\n",wait);
    exit(-1);
  }

  bool rd=true;

  if (strcasecmp(argv[2],"DISP")==0) {
    rd = false;
  }

  mi.test1(rd, coreid, wait);

  return 0;
}

