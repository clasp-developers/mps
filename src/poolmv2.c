/* impl.c.poolmv2: MANUAL VARIABLE POOL, II
 *
 * $HopeName: MMsrc!poolmv2.c(MMdevel_gavinm_mvff.1) $
 * Copyright (C) 1998 Harlequin Group plc.  All rights reserved.
 *
 * .readership: any MPS developer
 *
 * .purpose: A manual-variable pool designed to take advantage of
 *  placement according to predicted deathtime.
 *
 * .design: See design.mps.poolmv2
 */

#include "mpm.h"
#include "poolmv2.h"
#include "mpscmv2.h"
#include "abq.h"
#include "meter.h"

SRCID(poolmv2, "$HopeName: MMsrc!poolmv2.c(MMdevel_gavinm_mvff.1) $");


/* Signatures */

#define MV2Sig ((Sig)0x5193F299) /* SIGnature MV2 */


/* Private prototypes */

typedef struct MV2Struct *MV2;
static Res MV2Init(Pool pool, va_list arg);
static Bool MV2Check(MV2 mv2);
static void MV2Finish(Pool pool);
static Res MV2BufferFill(Seg *segReturn,
                         Addr *baseReturn, Addr *limitReturn,
                         Pool pool, Buffer buffer, Size minSize,
                         Bool withReservoirPermit);
static void MV2BufferEmpty(Pool pool, Buffer buffer);
static void MV2Free(Pool pool, Addr base, Size size);
static Res MV2Describe(Pool pool, mps_lib_FILE *stream);
static Res MV2SegAlloc(Seg *segReturn, MV2 mv2, Size size, Pool pool,
                       Bool withReservoirPermit);

static void MV2SegFree(MV2 mv2, Seg seg);
static Bool MV2ReturnBlockSegs(MV2 mv2, CBSBlock block, Arena arena);
static void MV2NoteNew(CBS cbs, CBSBlock block, Size oldSize, Size newSize);
static void MV2NoteDelete(CBS cbs, CBSBlock block, Size oldSize, Size newSize);
static void ABQRefillIfNecessary(MV2 mv2, Size size);
static Bool ABQRefillCallback(CBS cbs, CBSBlock block, void *closureP,
                              unsigned long closureS);
static Res MV2ContingencySearch(CBSBlock *blockReturn, CBS cbs,
                                Size min);
static Bool MV2ContingencyCallback(CBS cbs, CBSBlock block,
                                   void *closureP,
                                   unsigned long closureS);
static Bool MV2CheckFit(CBSBlock block, Size min, Arena arena);
static MV2 PoolPoolMV2(Pool pool);
static Pool MV2Pool(MV2 mv2);
static ABQ MV2ABQ(MV2 mv2);
static CBS MV2CBS(MV2 mv2);
static MV2 CBSMV2(CBS cbs);
static SegPref MV2SegPref(MV2 mv2);


/* Types */


typedef struct MV2Struct 
{
  PoolStruct poolStruct;
  CBSStruct cbsStruct;          /* The coalescing block structure */
  ABQStruct abqStruct;          /* The available block queue */
  SegPrefStruct segPrefStruct;  /* The preferences for segments */
  /* design.mps.poolmv2:arch.parameters */
  Size minSize;                 /* Pool parameter */
  Size meanSize;                /* Pool parameter */
  Size maxSize;                 /* Pool parameter */
  Count fragLimit;              /* Pool parameter */
  /* design.mps.poolmv2:arch.overview.abq.reuse.size */
  Size reuseSize;               /* Size at which blocks are recycled */
  /* design.mps.poolmv2:arch.ap.fill.size */
  Size fillSize;                /* Size of pool segments */
  /* design.mps.poolmv2:arch.contingency */
  Size availLimit;              /* Limit on available */
  /* design.mps.poolmv2:impl.c.free.merge.segment.overflow */
  Bool abqOverflow;             /* ABQ dropped some candidates */
  /* design.mps.poolmv2:arch.ap.no-fit.* */
  Bool splinter;                /* Saved splinter */
  Seg splinterSeg;              /* Saved splinter seg */
  Addr splinterBase;            /* Saved splinter base */
  Addr splinterLimit;           /* Saved splinter size */

  /* pool accounting --- one of these first four is redundant, but
     size and available are used to implement fragmentation policy */
  Size size;                    /* size of segs in pool */
  Size allocated;               /* bytes allocated to mutator */
  Size available;               /* bytes available for allocation */
  Size unavailable;             /* bytes lost to fragmentation */
  
  /* pool meters*/
  METER_DECL(segAllocs);
  METER_DECL(segFrees);
  METER_DECL(bufferFills);
  METER_DECL(bufferEmpties);
  METER_DECL(poolFrees);
  METER_DECL(poolSize);
  METER_DECL(poolAllocated);
  METER_DECL(poolAvailable);
  METER_DECL(poolUnavailable);
  METER_DECL(poolUtilization);
  /* abq meters */
  METER_DECL(finds);
  METER_DECL(overflows);
  METER_DECL(underflows);
  METER_DECL(refills);
  METER_DECL(refillPushes);
  METER_DECL(refillOverflows);
  METER_DECL(refillReturns);
  /* fragmentation meters */
  METER_DECL(perfectFits);
  METER_DECL(firstFits);
  METER_DECL(secondFits);
  METER_DECL(failures);
  /* contingency meters */
  METER_DECL(emergencyContingencies);
  METER_DECL(fragLimitContingencies);
  METER_DECL(contingencySearches);
  METER_DECL(contingencyHardSearches);
  /* splinter meters */
  METER_DECL(splinters);
  METER_DECL(splintersUsed);
  METER_DECL(splintersDropped);
  METER_DECL(sawdust);
  /* exception meters */
  METER_DECL(exceptions);
  METER_DECL(exceptionSplinters);
  METER_DECL(exceptionReturns);
  
  Sig sig;
} MV2Struct;


static PoolClassStruct PoolClassMV2Struct =
{
  PoolClassSig,
  "MV2",                        /* name */
  sizeof(MV2Struct),            /* size */
  offsetof(MV2Struct, poolStruct), /* offset */
  /* --- should we implement AttrALLOC? */
  AttrFREE | AttrBUF | AttrBUF_RESERVE,/* attr */
  MV2Init,                      /* init */
  MV2Finish,                    /* finish */
  PoolNoAlloc,                  /* alloc */
  MV2Free,                      /* free */
  PoolTrivBufferInit,           /* bufferInit */
  MV2BufferFill,                /* bufferFill */
  MV2BufferEmpty,               /* bufferEmpty */
  PoolTrivBufferFinish,         /* bufferFinish */
  PoolNoTraceBegin,             /* traceBegin */
  PoolNoAccess,                 /* access */
  PoolNoWhiten,                 /* whiten */
  PoolNoGrey,                   /* mark */
  PoolNoBlacken,                /* blacken */
  PoolNoScan,                   /* scan */
  PoolNoFix,                    /* fix */
  PoolNoFix,                    /* emergency fix */
  PoolNoReclaim,                /* relcaim */
  PoolNoBenefit,                /* benefit */
  PoolNoAct,                    /* act */
  PoolNoRampBegin,
  PoolNoRampEnd,
  PoolNoWalk,                   /* walk */
  MV2Describe,                  /* describe */
  PoolClassSig                  /* impl.h.mpmst.class.end-sig */
};


/* Macros */


/* .trans.something the C language sucks */
#define unless(cond) if (!(cond))
#define when(cond) if (cond)


/* Accessors */


static MV2 PoolPoolMV2(Pool pool) 
{
  return PARENT(MV2Struct, poolStruct, pool);
}


static Pool MV2Pool(MV2 mv2) 
{
  return &mv2->poolStruct;
}


static ABQ MV2ABQ(MV2 mv2)
{
  return &mv2->abqStruct;
}


static CBS MV2CBS(MV2 mv2) 
{
  return &mv2->cbsStruct;
}


static MV2 CBSMV2(CBS cbs)
{
  return PARENT(MV2Struct, cbsStruct, cbs);
}


static SegPref MV2SegPref(MV2 mv2)
{
  return &mv2->segPrefStruct;
}


/* Methods */


/* MV2Init -- initialize an MV2 pool
 *
 * Parameters are:
 * minSize, meanSize, maxSize, reserveDepth, fragLimit
 */
static Res MV2Init(Pool pool, va_list arg)
{
  Arena arena;
  Size minSize, meanSize, maxSize, reuseSize, fillSize;
  Count reserveDepth, abqDepth, fragLimit;
  MV2 mv2;
  Res res;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  /* can't AVERT mv2, yet */
  arena = PoolArena(pool);
  AVERT(Arena, arena);
  
  /* --- Should there be a ResBADARG ? */
  minSize = va_arg(arg, Size);
  unless (minSize > 0)
    return ResLIMIT;
  meanSize = va_arg(arg, Size);
  unless (meanSize >= minSize)
    return ResLIMIT;
  maxSize = va_arg(arg, Size);
  unless (maxSize >= meanSize)
    return ResLIMIT;
  /* --- check that maxSize is not too large */
  reserveDepth = va_arg(arg, Count);
  unless (reserveDepth > 0)
    return ResLIMIT;
  /* --- check that reserveDepth is not too large or small */
  fragLimit = va_arg(arg, Count);
  unless (fragLimit >= 0 && fragLimit <= 100)
    return ResLIMIT;

  /* see design.mps.poolmv2:arch.parameters */
  fillSize = SizeAlignUp(maxSize, ArenaAlign(arena));
  /* see design.mps.poolmv2:arch.fragmentation.internal */
  reuseSize = 2 * fillSize;
  abqDepth = (reserveDepth * meanSize + reuseSize - 1) / reuseSize;
  /* keep the abq from being useless */
  if (abqDepth < 3)
    abqDepth = 3;

  res = CBSInit(arena, MV2CBS(mv2), &MV2NoteNew, &MV2NoteDelete,
                NULL, NULL, reuseSize, TRUE);
  if (res != ResOK)
    goto failCBS;
  
  res = ABQInit(arena, MV2ABQ(mv2), abqDepth);
  if (res != ResOK)
    goto failABQ;

  {
    RefSet refset;
    /* --- Loci needed here, what should the pref be? */
    /* --- why not SegPrefDefault(MV2SegPref)? */
    *MV2SegPref(mv2) = *SegPrefDefault();
    /* +++ Get own RefSet */
    refset = RefSetComp(ARENA_DEFAULT_REFSET);
    SegPrefExpress(MV2SegPref(mv2), SegPrefRefSet, (void *)&refset);
  }

  mv2->reuseSize = reuseSize;
  mv2->fillSize = fillSize;
  mv2->abqOverflow = FALSE;
  mv2->minSize = minSize;
  mv2->meanSize = meanSize;
  mv2->maxSize = maxSize;
  mv2->fragLimit = fragLimit;
  mv2->splinter = FALSE;
  mv2->splinterSeg = NULL;
  mv2->splinterBase = (Addr)0;
  mv2->splinterLimit = (Addr)0;
  
  /* accounting */
  mv2->size = 0;
  mv2->allocated = 0;
  mv2->available = 0;
  mv2->availLimit = 0;
  mv2->unavailable = 0;
  
  /* meters*/
  METER_INIT(mv2->segAllocs, "segment allocations");
  METER_INIT(mv2->segFrees, "segment frees");
  METER_INIT(mv2->bufferFills, "buffer fills");
  METER_INIT(mv2->bufferEmpties, "buffer empties");
  METER_INIT(mv2->poolFrees, "pool frees");
  METER_INIT(mv2->poolSize, "pool size");
  METER_INIT(mv2->poolAllocated, "pool allocated");
  METER_INIT(mv2->poolAvailable, "pool available");
  METER_INIT(mv2->poolUnavailable, "pool unavailable");
  METER_INIT(mv2->poolUtilization, "pool utilization");
  METER_INIT(mv2->finds, "ABQ finds");
  METER_INIT(mv2->overflows, "ABQ overflows");
  METER_INIT(mv2->underflows, "ABQ underflows");
  METER_INIT(mv2->refills, "ABQ refills");
  METER_INIT(mv2->refillPushes, "ABQ refill pushes");
  METER_INIT(mv2->refillOverflows, "ABQ refill overflows");
  METER_INIT(mv2->refillReturns, "ABQ refill returns");
  METER_INIT(mv2->perfectFits, "perfect fits");
  METER_INIT(mv2->firstFits, "first fits");
  METER_INIT(mv2->secondFits, "second fits");
  METER_INIT(mv2->failures, "failures");
  METER_INIT(mv2->emergencyContingencies, "emergency contingencies");
  METER_INIT(mv2->fragLimitContingencies,
             "fragmentation limit contingencies");
  METER_INIT(mv2->contingencySearches, "contingency searches");
  METER_INIT(mv2->contingencyHardSearches,
             "contingency hard searches");
  METER_INIT(mv2->splinters, "splinters");
  METER_INIT(mv2->splintersUsed, "splinters used");
  METER_INIT(mv2->splintersDropped, "splinters dropped");
  METER_INIT(mv2->sawdust, "sawdust");
  METER_INIT(mv2->exceptions, "exceptions");
  METER_INIT(mv2->exceptionSplinters, "exception splinters");
  METER_INIT(mv2->exceptionReturns, "exception returns");

  mv2->sig = MV2Sig;

  AVERT(MV2, mv2);
  return ResOK;

failABQ:
  CBSFinish(MV2CBS(mv2));
failCBS:
  AVER(res != ResOK);
  return res;
}


/* MV2Check -- validate an MV2 Pool
 */
static Bool MV2Check(MV2 mv2)
{
  CHECKS(MV2, mv2);
  CHECKD(Pool, &mv2->poolStruct);
  CHECKL(mv2->poolStruct.class == &PoolClassMV2Struct);
  CHECKD(CBS, &mv2->cbsStruct);
  /* CHECKL(CBSCheck(MV2CBS(mv2))); */
  CHECKD(ABQ, &mv2->abqStruct);
  /* CHECKL(ABQCheck(MV2ABQ(mv2))); */
  CHECKD(SegPref, &mv2->segPrefStruct);
  CHECKL(mv2->reuseSize >= 2 * mv2->fillSize);
  CHECKL(mv2->fillSize >= mv2->maxSize);
  CHECKL(mv2->maxSize >= mv2->meanSize);
  CHECKL(mv2->meanSize >= mv2->minSize);
  CHECKL(mv2->minSize > 0);
  CHECKL(mv2->fragLimit >= 0 && mv2->fragLimit <= 100);
  CHECKL(mv2->availLimit == mv2->size * mv2->fragLimit / 100);
  CHECKL(BoolCheck(mv2->abqOverflow));
  CHECKL(BoolCheck(mv2->splinter));
  if (mv2->splinter) {
    CHECKL(AddrOffset(mv2->splinterBase, mv2->splinterLimit) >=
           mv2->minSize);
    /* CHECKD(Seg, mv2->splinterSeg); */
    CHECKL(SegCheck(mv2->splinterSeg));
    CHECKL(mv2->splinterBase >= SegBase(mv2->splinterSeg));
    CHECKL(mv2->splinterLimit <= SegLimit(mv2->splinterSeg));
  }
  CHECKL(mv2->size == mv2->allocated + mv2->available +
         mv2->unavailable);
  /* --- could check that sum of segment sizes == mv2->size */
  
  /* --- check meters? */

  return TRUE;
}


/* MV2Finish -- finish an MV2 pool
 */
static void MV2Finish(Pool pool)
{
  MV2 mv2;
  Arena arena;
  Ring ring;
  Ring node, nextNode;
  
  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);
  arena = PoolArena(pool);
  AVERT(Arena, arena);

  EVENT_P(MV2Finish, mv2);

  /* Free the segments in the pool */
  ring = PoolSegRing(pool);
  RING_FOR(node, ring, nextNode) {
    MV2SegFree(mv2, SegOfPoolRing(node));
  }

  /* Finish the ABQ and CBS structures */
  ABQFinish(arena, MV2ABQ(mv2));
  CBSFinish(MV2CBS(mv2));

  mv2->sig = SigInvalid;
}


/* MV2BufferFill -- refill an allocation buffer from an MV2 pool
 *
 * See design.mps.poolmv2:impl.c.ap.fill
 */
static Res MV2BufferFill(Seg *segReturn,
                         Addr *baseReturn, Addr *limitReturn,
                         Pool pool, Buffer buffer, Size minSize,
                         Bool withReservoirPermit)
{
  Seg seg;
  MV2 mv2;
  Res res;
  Addr base, limit;
  Arena arena;
  Size alignedSize, fillSize;
  CBSBlock block;

  AVER(segReturn != NULL);
  AVER(baseReturn != NULL);
  AVER(limitReturn != NULL);
  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);
  AVERT(Buffer, buffer);
  AVER(BufferIsReset(buffer));
  AVER(minSize > 0);
  AVER(SizeIsAligned(minSize, pool->alignment));
  AVER(BoolCheck(withReservoirPermit));

  arena = PoolArena(pool);
  fillSize = mv2->fillSize;
  alignedSize = SizeAlignUp(minSize, ArenaAlign(arena));

  /* design.mps.poolmv2:arch.ap.no-fit.oversize */
  /* Allocate oversize blocks exactly, directly from the arena */
  if (minSize > fillSize) {
    res = MV2SegAlloc(&seg, mv2, alignedSize, pool, withReservoirPermit);
    if (res == ResOK) {
      base = SegBase(seg);
      /* only allocate this block in the segment */
      limit = AddrAdd(base, minSize);
      mv2->available -= alignedSize - minSize;
      mv2->unavailable += alignedSize - minSize;
      AVER(mv2->size == mv2->allocated + mv2->available +
           mv2->unavailable);
      METER_ACC(mv2->exceptions, minSize);
      METER_ACC(mv2->exceptionSplinters, alignedSize - minSize);
      goto done;
    }
    /* --- There cannot be a segment big enough to hold this object in
       the free list, although there may be segments that could be
       coalesced to do so. */
    AVER(res != ResOK);
    return res;
  }

  /* design.mps.poolmv2:arch.ap.no-fit.return */
  /* Use any splinter, if available */
  if (mv2->splinter) {
    base = mv2->splinterBase;
    limit = mv2->splinterLimit;
    if(AddrOffset(base, limit) >= minSize) {
      seg = mv2->splinterSeg;
      mv2->splinter = FALSE;
      METER_ACC(mv2->splintersUsed, AddrOffset(base, limit));
      goto done;
    }
  }
  
  /* Attempt to retrieve a free block from the ABQ */
  ABQRefillIfNecessary(mv2, minSize);
  res = ABQPeek(MV2ABQ(mv2), &block);
  if (res != ResOK) {
    METER_ACC(mv2->underflows, minSize);
    /* design.mps.poolmv2:arch.contingency.fragmentation-limit */
    if (mv2->available >=  mv2->availLimit) {
      METER_ACC(mv2->fragLimitContingencies, minSize);
      res = MV2ContingencySearch(&block, MV2CBS(mv2), minSize);
    }
  } else {
    METER_ACC(mv2->finds, minSize);
  }
found:
  if (res == ResOK) {
    base = CBSBlockBase(block);
    limit = CBSBlockLimit(block);
    {
      Bool b = SegOfAddr(&seg, arena, base);
      AVER(b);
    }
    /* Only pass out segments --- may not be the best long-term policy
     */
    {
      Addr segLimit = SegLimit(seg);

      if (limit <= segLimit) {
        /* perfect fit */
        METER_ACC(mv2->perfectFits, AddrOffset(base, limit));
      } else if (AddrOffset(base, segLimit) >= minSize) {
        /* fit in 1st segment */
        limit = segLimit;
        METER_ACC(mv2->firstFits, AddrOffset(base, limit));
      } else {
        /* fit in 2nd second segment */
        base = segLimit;
        {
          Bool b = SegOfAddr(&seg, arena, base);
          AVER(b);
        }
        segLimit = SegLimit(seg);
        if (limit > segLimit)
          limit = segLimit;
        METER_ACC(mv2->secondFits, AddrOffset(base, limit));
      }
    }
    {
      Res r = CBSDelete(MV2CBS(mv2), base, limit);
      AVER(r == ResOK);
    }
    goto done;
  }
  
  /* Attempt to request a block from the arena */
  /* see design.mps.poolmv2:impl.c.free.merge.segment */
  res = MV2SegAlloc(&seg, mv2, fillSize, pool, withReservoirPermit);
  if (res == ResOK) {
    base = SegBase(seg);
    limit = SegLimit(seg);
    goto done;
  }
  
  /* Try contingency */
  METER_ACC(mv2->emergencyContingencies, minSize);
  res = MV2ContingencySearch(&block, MV2CBS(mv2), minSize);
  if (res == ResOK){
    goto found;
  }

  /* --- ask other pools to free reserve and retry */
  METER_ACC(mv2->failures, minSize);
  AVER(res != ResOK);
  return res;
  
done:
  *segReturn = seg;
  *baseReturn = base;
  *limitReturn = limit;
  mv2->available -= AddrOffset(base, limit);
  mv2->allocated += AddrOffset(base, limit);
  AVER(mv2->size == mv2->allocated + mv2->available +
       mv2->unavailable);
  METER_ACC(mv2->poolUtilization, mv2->allocated * 100 / mv2->size);
  METER_ACC(mv2->poolUnavailable, mv2->unavailable);
  METER_ACC(mv2->poolAvailable, mv2->available);
  METER_ACC(mv2->poolAllocated, mv2->allocated);
  METER_ACC(mv2->poolSize, mv2->size);
  METER_ACC(mv2->bufferFills, AddrOffset(base, limit));
  EVENT_PPWAW(MV2BufferFill, mv2, buffer, minSize, base,
              AddrOffset(base, limit));
  AVER(AddrOffset(base, limit) >= minSize);
  return ResOK;
}


/* MV2BufferEmpty -- return an unusable portion of a buffer to the MV2
 * pool
 *
 * See design.mps.poolmv2:impl.c.ap.empty
 */
static void MV2BufferEmpty(Pool pool, Buffer buffer)
{
  MV2 mv2;
  Seg seg;
  Addr base, limit;
  Size size;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);
  AVERT(Buffer, buffer);
  AVER(!BufferIsReset(buffer));
  AVER(BufferIsReady(buffer));

  seg = BufferSeg(buffer);
  base = BufferGetInit(buffer);
  limit = BufferLimit(buffer);
  size = AddrOffset(base, limit);
  
  EVENT_PPW(MV2BufferEmpty, mv2, buffer, size);
  if (size == 0)
    return;

  mv2->available += size;
  mv2->allocated -= size;
  AVER(mv2->size == mv2->allocated + mv2->available +
       mv2->unavailable);
  METER_ACC(mv2->poolUtilization, mv2->allocated * 100 / mv2->size);
  METER_ACC(mv2->poolUnavailable, mv2->unavailable);
  METER_ACC(mv2->poolAvailable, mv2->available);
  METER_ACC(mv2->poolAllocated, mv2->allocated);
  METER_ACC(mv2->poolSize, mv2->size);
  METER_ACC(mv2->bufferEmpties, size);

  /* design.mps.poolmv2:arch.ap.no-fit.splinter */
  if (size < mv2->minSize) {
    Res res = CBSInsert(MV2CBS(mv2), base, limit);
    AVER(res == ResOK);
    METER_ACC(mv2->sawdust, size);
    return;
  }

  METER_ACC(mv2->splinters, size);
  /* design.mps.poolmv2:arch.ap.no-fit.return */
  if (mv2->splinter) {
    Size oldSize = AddrOffset(mv2->splinterBase, mv2->splinterLimit);

    /* Old better, drop new */
    if (size < oldSize) {
      Res res = CBSInsert(MV2CBS(mv2), base, limit);
      AVER(res == ResOK);
      METER_ACC(mv2->splintersDropped, size);
      return;
    } else {
      /* New better, drop old */
      Res res = CBSInsert(MV2CBS(mv2), mv2->splinterBase,
			  mv2->splinterLimit);
      AVER(res == ResOK);
      METER_ACC(mv2->splintersDropped, oldSize);
    }
  }

  mv2->splinter = TRUE;
  mv2->splinterSeg = seg;
  mv2->splinterBase = base;
  mv2->splinterLimit = limit;
}


/* MV2Free -- free a block (previously allocated from a buffer) that
 * is no longer in use
 *
 * see design.poolmv2.impl.c.free
 */
static void MV2Free(Pool pool, Addr base, Size size)
{ 
  MV2 mv2;
  Addr limit;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);
  AVER(base != (Addr)0);
  AVER(size > 0);


  /* We know the buffer observes pool->alignment  */
  size = SizeAlignUp(size, pool->alignment);
  limit = AddrAdd(base, size);
  METER_ACC(mv2->poolFrees, size);
  mv2->available += size;
  mv2->allocated -= size;
  AVER(mv2->size == mv2->allocated + mv2->available +
       mv2->unavailable);
  METER_ACC(mv2->poolUtilization, mv2->allocated * 100 / mv2->size);
  METER_ACC(mv2->poolUnavailable, mv2->unavailable);
  METER_ACC(mv2->poolAvailable, mv2->available);
  METER_ACC(mv2->poolAllocated, mv2->allocated);
  METER_ACC(mv2->poolSize, mv2->size);
  
  /* design.mps.poolmv2:arch.ap.no-fit.oversize.policy */
  /* Return exceptional blocks directly to arena */
  if (size > mv2->fillSize) {
    Seg seg;
    {
      Bool b = SegOfAddr(&seg, PoolArena(pool), base);
      AVER(b);
    }
    AVER(base == SegBase(seg));
    AVER(limit <= SegLimit(seg));
    mv2->available += SegSize(seg) - size;
    mv2->unavailable -= SegSize(seg) - size;
    AVER(mv2->size == mv2->allocated + mv2->available +
         mv2->unavailable);
    METER_ACC(mv2->exceptionReturns, SegSize(seg));
    if (SegBuffer(seg) != NULL)
      BufferDetach(SegBuffer(seg), MV2Pool(mv2));
    MV2SegFree(mv2, seg);
    return;
  }
  
  {
    Res res = CBSInsert(MV2CBS(mv2), base, limit);
    AVER(res == ResOK);
  }
}


/* MV2Describe -- describe an MV2 pool
 */
static Res MV2Describe(Pool pool, mps_lib_FILE *stream)
{
  Res res;
  MV2 mv2;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);
  AVER(stream != NULL);

  res = WriteF(stream,
	       "MV2 $P\n{\n", (WriteFP)mv2,
	       "  minSize: $U \n", (WriteFU)mv2->minSize,
	       "  meanSize: $U \n", (WriteFU)mv2->meanSize,
	       "  maxSize: $U \n", (WriteFU)mv2->maxSize,
	       "  fragLimit: $U \n", (WriteFU)mv2->fragLimit,
	       "  reuseSize: $U \n", (WriteFU)mv2->reuseSize,
	       "  fillSize: $U \n", (WriteFU)mv2->fillSize,
	       "  availLimit: $U \n", (WriteFU)mv2->availLimit,
	       "  abqOverflow: $S \n", mv2->abqOverflow?"TRUE":"FALSE",
	       "  splinter: $S \n", mv2->splinter?"TRUE":"FALSE",
	       "  splinterSeg: $P \n", (WriteFP)mv2->splinterSeg,
	       "  splinterBase: $A \n", (WriteFA)mv2->splinterBase,
	       "  splinterLimit: $A \n", (WriteFU)mv2->splinterLimit,
	       "  size: $U \n", (WriteFU)mv2->size,
	       "  allocated: $U \n", (WriteFU)mv2->allocated,
	       "  available: $U \n", (WriteFU)mv2->available,
	       "  unavailable: $U \n", (WriteFU)mv2->unavailable,
	       NULL);
  if(res != ResOK)
    return res;

  res = CBSDescribe(MV2CBS(mv2), stream);
  if(res != ResOK)
    return res;

  res = ABQDescribe(MV2ABQ(mv2), stream);
  if(res != ResOK)
    return res;

  /* --- deal with non-Ok res's */
  METER_WRITE(mv2->segAllocs, stream);
  METER_WRITE(mv2->segFrees, stream);
  METER_WRITE(mv2->bufferFills, stream);
  METER_WRITE(mv2->bufferEmpties, stream);
  METER_WRITE(mv2->poolFrees, stream);
  METER_WRITE(mv2->poolSize, stream);
  METER_WRITE(mv2->poolAllocated, stream);
  METER_WRITE(mv2->poolAvailable, stream);
  METER_WRITE(mv2->poolUnavailable, stream);
  METER_WRITE(mv2->poolUtilization, stream);
  METER_WRITE(mv2->finds, stream);
  METER_WRITE(mv2->overflows, stream);
  METER_WRITE(mv2->underflows, stream);
  METER_WRITE(mv2->refills, stream);
  METER_WRITE(mv2->refillPushes, stream);
  METER_WRITE(mv2->refillOverflows, stream);
  METER_WRITE(mv2->refillReturns, stream);
  METER_WRITE(mv2->perfectFits, stream);
  METER_WRITE(mv2->firstFits, stream);
  METER_WRITE(mv2->secondFits, stream);
  METER_WRITE(mv2->failures, stream);
  METER_WRITE(mv2->emergencyContingencies, stream);
  METER_WRITE(mv2->fragLimitContingencies, stream);
  METER_WRITE(mv2->contingencySearches, stream);
  METER_WRITE(mv2->contingencyHardSearches, stream);
  METER_WRITE(mv2->splinters, stream);
  METER_WRITE(mv2->splintersUsed, stream);
  METER_WRITE(mv2->splintersDropped, stream);
  METER_WRITE(mv2->sawdust, stream);
  METER_WRITE(mv2->exceptions, stream);
  METER_WRITE(mv2->exceptionSplinters, stream);
  METER_WRITE(mv2->exceptionReturns, stream);
  
  res = WriteF(stream, "}\n", NULL);
  if(res != ResOK)
    return res;

  return ResOK;               
}


/* Pool Interface */


/* PoolClassMV2 -- the Pool (sub-)Class for an MV2 pool
 */
PoolClass PoolClassMV2(void)
{
  return &PoolClassMV2Struct;
}


/* MPS Interface */


/*
 * mps_class_mv2 -- the class of an mv2 pool
 */
mps_class_t mps_class_mv2(void)
{
  return (mps_class_t)(PoolClassMV2());
}


/* MPS Interface extensions --- should these be pool generics? */


/* mps_mv2_size -- number of bytes committed to the pool
 */
size_t mps_mv2_size(mps_pool_t mps_pool)
{
  Pool pool;
  MV2 mv2;

  pool = (Pool)mps_pool;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);

  return (size_t)mv2->size;
} 


/* mps_mv2_free_size -- number of bytes comitted to the pool that are
 * available for allocation
 */
size_t mps_mv2_free_size(mps_pool_t mps_pool)
{
  Pool pool;
  MV2 mv2;

  pool = (Pool)mps_pool;

  AVERT(Pool, pool);
  mv2 = PoolPoolMV2(pool);
  AVERT(MV2, mv2);

  return (size_t)mv2->available;
}


/* Internal methods */


/* MV2SegAlloc -- encapsulates SegAlloc with associated accounting and
 * metering
 */
static Res MV2SegAlloc(Seg *segReturn, MV2 mv2, Size size, 
                       Pool pool, Bool withReservoirPermit)
{
  Res res = SegAlloc(segReturn, MV2SegPref(mv2), size, pool,
                     withReservoirPermit);

  if (res == ResOK) {
    Size segSize = SegSize(*segReturn);
    
    /* see design.mps.poolmv2:arch.fragmentation.internal */
    AVER(segSize >= mv2->fillSize);
    mv2->size += segSize;
    mv2->available += segSize;
    mv2->availLimit = mv2->size * mv2->fragLimit / 100;
    AVER(mv2->size == mv2->allocated + mv2->available +
         mv2->unavailable);
    METER_ACC(mv2->segAllocs, segSize);
  }
  return res;
}
  

/* MV2SegFree -- encapsulates SegFree with associated accounting and
 * metering
 */
static void MV2SegFree(MV2 mv2, Seg seg) 
{
  Size size = SegSize(seg);

  mv2->available -= size;
  mv2->size -= size;
  mv2->availLimit = mv2->size * mv2->fragLimit / 100;
  AVER(mv2->size == mv2->allocated + mv2->available +
       mv2->unavailable);
  SegFree(seg);
  METER_ACC(mv2->segFrees, size);
}


/* MV2ReturnBlockSegs -- return (interior) segments of a block to the
 * arena
 */
static Bool MV2ReturnBlockSegs(MV2 mv2, CBSBlock block, Arena arena) 
{
  Addr base, limit;
  Bool success = FALSE;
    
  base = CBSBlockBase(block);
  limit = CBSBlockLimit(block);

  while (base < limit) {
    Seg seg;
    Addr segBase, segLimit;
      
    {
      Bool b = SegOfAddr(&seg, arena, base);
      AVER(b);
    }
    segBase = SegBase(seg);
    segLimit = SegLimit(seg);
    if (base <= segBase && limit >= segLimit) {
      {
        Res r = CBSDelete(MV2CBS(mv2), segBase, segLimit);
        AVER(r == ResOK);
      }
      MV2SegFree(mv2, seg);
      success = TRUE;
    }
    base = segLimit;
  }

  return success;
}


/* MV2NoteNew -- callback invoked when a block on the CBS >= reuseSize
 */
static void MV2NoteNew(CBS cbs, CBSBlock block, Size oldSize, Size newSize) 
{
  Res res;
  MV2 mv2;
  
  AVERT(CBS, cbs);
  mv2 = CBSMV2(cbs);
  AVERT(MV2, mv2);
  AVERT(CBSBlock, block);
  AVER(CBSBlockSize(block) >= mv2->reuseSize);
  UNUSED(oldSize);
  UNUSED(newSize);

  res = ABQPush(MV2ABQ(mv2), block);
  /* See design.mps.poolmv2:impl.c.free.merge */
  if (res != ResOK) {
    Arena arena = PoolArena(MV2Pool(mv2));
    CBSBlock oldBlock;
    {
      Res r = ABQPeek(MV2ABQ(mv2), &oldBlock);
      AVER(r == ResOK);
    }
    /* --- This should always succeed */
    (void)MV2ReturnBlockSegs(mv2, oldBlock, arena);
    res = ABQPush(MV2ABQ(CBSMV2(cbs)), block);
    if (res != ResOK) {
      unless(MV2ReturnBlockSegs(mv2, block, arena)) {
        mv2->abqOverflow = TRUE;
        METER_ACC(mv2->overflows, CBSBlockSize(block));
      }
    }
  }
}


/* MV2NoteDelete -- callback invoked when a block on the CBS <=
 * reuseSize
 */
static void MV2NoteDelete(CBS cbs, CBSBlock block, Size oldSize, Size newSize)
{
  AVERT(CBS, cbs);
  AVERT(MV2, CBSMV2(cbs));
  AVERT(CBSBlock, block);
  AVER(CBSBlockSize(block) < CBSMV2(cbs)->reuseSize);
  UNUSED(oldSize);
  UNUSED(newSize);
  
  {
    Res res = ABQDelete(MV2ABQ(CBSMV2(cbs)), block);
    AVER(res == ResOK || CBSMV2(cbs)->abqOverflow);
  }
}


/* ABQRefillIfNecessary -- refill the ABQ from the CBS if it had
 * overflown and is now empty
 */
static void ABQRefillIfNecessary(MV2 mv2, Size size) 
{
  AVERT(MV2, mv2);
  AVER(size > 0);

  if (mv2->abqOverflow && ABQIsEmpty(MV2ABQ(mv2))) {
    mv2->abqOverflow = FALSE;
    METER_ACC(mv2->refills, size);
    CBSIterateLarge(MV2CBS(mv2), &ABQRefillCallback, NULL, 0);
  }
}


/* ABQRefillCallback -- called from CBSIterate at the behest of
 * ABQRefillIfNecessary
 */
static Bool ABQRefillCallback(CBS cbs, CBSBlock block, void *closureP,
                              unsigned long closureS)
{
  Res res;
  MV2 mv2;
  
  AVERT(CBS, cbs);
  mv2 = CBSMV2(cbs);
  AVERT(MV2, mv2);
  AVERT(ABQ, MV2ABQ(mv2));
  AVERT(CBSBlock, block);
  AVER(CBSBlockSize(block) >= mv2->reuseSize);
  UNUSED(closureP);
  UNUSED(closureS);

  METER_ACC(mv2->refillPushes, ABQDepth(MV2ABQ(mv2)));
  res = ABQPush(MV2ABQ(mv2), block);
  if (res != ResOK) {
    if (MV2ReturnBlockSegs(mv2, block, PoolArena(MV2Pool(mv2)))) {
      METER_ACC(mv2->refillReturns, CBSBlockSize(block));
      return TRUE;
    } else {
      mv2->abqOverflow = TRUE;
      METER_ACC(mv2->refillOverflows, CBSBlockSize(block));
      return FALSE;
    }
  }

  return TRUE;
}
  

/* Closure for MV2ContingencySearch */
typedef struct MV2ContigencyStruct *MV2Contigency;

typedef struct MV2ContigencyStruct 
{
  CBSBlock blockReturn;
  Arena arena;
  Size min;
  /* meters */
  Count steps;
  Count hardSteps;
} MV2ContigencyStruct;


/* MV2ContingencySearch -- search the CBS for a block of size min
 */
static Res MV2ContingencySearch(CBSBlock *blockReturn, CBS cbs,
                                Size min)
{
  MV2ContigencyStruct cls;

  cls.blockReturn = NULL;
  cls.arena = PoolArena(MV2Pool(CBSMV2(cbs)));
  cls.min = min;
  cls.steps = 0;
  cls.hardSteps = 0;
  
  CBSIterate(cbs, &MV2ContingencyCallback, (void *)&cls,
             (unsigned long)sizeof(cls));
  if (cls.blockReturn != NULL) {
    AVER(CBSBlockSize(cls.blockReturn) >= min);
    METER_ACC(CBSMV2(cbs)->contingencySearches, cls.steps);
    if (cls.hardSteps) {
      METER_ACC(CBSMV2(cbs)->contingencyHardSearches, cls.hardSteps);
    }
    *blockReturn = cls.blockReturn;
    return ResOK;
  }
    
  return ResFAIL;
}


/* MV2ContingencyCallback -- called from CBSIterate at the behest of
 * MV2ContingencySearch
 */
static Bool MV2ContingencyCallback(CBS cbs, CBSBlock block,
                                   void *closureP,
                                   unsigned long closureS)
{
  MV2Contigency cl;
  Size size;
  
  AVERT(CBS, cbs);
  AVERT(CBSBlock, block);
  AVER(closureP != NULL);
  AVER(closureS == sizeof(MV2ContigencyStruct));

  cl = (MV2Contigency)closureP;
  size = CBSBlockSize(block);
  
  cl->steps++;
  if (size < cl->min)
    return TRUE;

  /* verify that min will fit when seg-aligned */
  if (size >= 2 * cl->min) {
    cl->blockReturn = block;
    return FALSE;
  }
  
  /* do it the hard way */
  cl->hardSteps++;
  if (MV2CheckFit(block, cl->min, cl->arena)) {
    cl->blockReturn = block;
    return FALSE;
  }
  
  /* keep looking */
  return TRUE;
}


/* MV2CheckFit -- verify that segment-aligned block of size min can
 * fit in a candidate CBSblock
 */
static Bool MV2CheckFit(CBSBlock block, Size min, Arena arena)
{
  Addr base = CBSBlockBase(block);
  Addr limit = CBSBlockLimit(block);
  Seg seg;
  Addr segLimit;
  {
    Bool b = SegOfAddr(&seg, arena, base);
    AVER(b);
  }
  segLimit = SegLimit(seg);

  if (limit <= segLimit) {
    if (AddrOffset(base, limit) >= min)
      return TRUE;
  }

  if (AddrOffset(base, segLimit) >= min)
    return TRUE;

  base = segLimit;
  {
    Bool b = SegOfAddr(&seg, arena, base);
    AVER(b);
  }
  segLimit = SegLimit(seg);

  if (AddrOffset(base, limit < segLimit ? limit : segLimit) >= min)
    return TRUE;

  return FALSE;
}
