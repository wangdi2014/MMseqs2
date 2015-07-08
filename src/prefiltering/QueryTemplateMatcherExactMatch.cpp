//
// Created by mad on 5/26/15.
//
#include "QueryTemplateMatcherExactMatch.h"
#include "QueryScoreLocal.h"

QueryTemplateMatcherExactMatch::QueryTemplateMatcherExactMatch(BaseMatrix *m, IndexTable *indexTable,
                                                               unsigned int *seqLens, short kmerThr,
                                                               double kmerMatchProb, int kmerSize, size_t dbSize,
                                                               unsigned int maxSeqLen)
        : QueryTemplateMatcher(m, indexTable, seqLens, kmerThr, kmerMatchProb, kmerSize, dbSize, false, maxSeqLen) {
    this->resList = (hit_t *) mem_align(ALIGN_INT, QueryScore::MAX_RES_LIST_LEN * sizeof(hit_t) );
    this->binData = new unsigned int[BIN_COUNT * BIN_SIZE];
    this->counterOutput = new CounterResult[MAX_DB_MATCHES];
    memset(counterOutput, 0, sizeof(CounterResult) * MAX_DB_MATCHES);
    this->counter = new CountInt32Array(dbSize, BIN_SIZE/2 );
    // needed for p-value calc.
    this->mu = kmerMatchProb;
    this->sqrtMu = sqrt(kmerMatchProb);
}

QueryTemplateMatcherExactMatch::~QueryTemplateMatcherExactMatch(){
    free(resList);
    delete [] binData;
    delete [] counterOutput;
    delete counter;
}

size_t QueryTemplateMatcherExactMatch::evaluateBins(CounterResult *output, size_t N) {
    size_t localResultSize = 0;
    localResultSize += counter->countElements(binData, N, output + localResultSize);
    return localResultSize;
}


std::pair<hit_t *, size_t> QueryTemplateMatcherExactMatch::matchQuery (Sequence * seq, unsigned int identityId){
    seq->resetCurrPos();
    match(seq);
    return getResult(seq->L, identityId, 0);
}

void QueryTemplateMatcherExactMatch::match(Sequence* seq){
    // go through the query sequence
    size_t kmerListLen = 0;
    size_t numMatches = 0;
    //size_t pos = 0;
    stats->diagonalOverflow = false;
    unsigned int * sequenceHits = binData;
    unsigned int * lastSequenceHit = binData + MAX_DB_MATCHES;
    size_t seqListSize;
    while(seq->hasNextKmer()){
        const int* kmer = seq->nextKmer();
        const ScoreMatrix kmerList = kmerGenerator->generateKmerList(kmer);
        kmerListLen += kmerList.elementSize;
        const unsigned short i =  seq->getCurrentPosition();
        // match the index table
        for (unsigned int kmerPos = 0; kmerPos < kmerList.elementSize; kmerPos++) {
            // generate k-mer list
            const IndexEntryLocal *entries = indexTable->getDBSeqList<IndexEntryLocal>(kmerList.index[kmerPos],
                                                                                       &seqListSize);
            // detected overflow while matching TODO smarter way
            seqListSize = (sequenceHits + seqListSize >= lastSequenceHit) ? 0 : seqListSize;

            for (unsigned int seqIdx = 0; LIKELY(seqIdx < seqListSize); seqIdx++) {
                IndexEntryLocal entry = entries[seqIdx];
                const unsigned char j = entry.position_j;
                const unsigned int seqId = entry.seqId;
                const unsigned char diagonal = (i - j);
                const unsigned int diagonalBin = diagonal/static_cast<unsigned char>(16);
                // concat diagonal to bit 28 - 31 (4 bit)
                *sequenceHits = seqId | (diagonalBin << 28) ;
                sequenceHits++;
            }
            numMatches += seqListSize;
        }
    }

    //fill the output
    size_t doubleMatches = evaluateBins(counterOutput, numMatches);
    stats->doubleMatches = doubleMatches;
    stats->kmersPerPos   = ((double)kmerListLen/(double)seq->L);
    stats->querySeqLen   = seq->L;
    stats->dbMatches     = numMatches;
}


std::pair<hit_t *, size_t>  QueryTemplateMatcherExactMatch::getResult(const int l,
                                                                      const unsigned int id,
                                                                      const unsigned short thr) {
    size_t elementCounter = 0;
    size_t resultSize = stats->doubleMatches;
    //std::sort(foundSequences, foundSequences + resultSize);
    std::sort(counterOutput, counterOutput + resultSize, compareCounterResult); // TODO
    if (id != UINT_MAX){
        hit_t * result = (resList + 0);
        const unsigned short rawScore  = l;
        result->seqId = id;
        result->prefScore = rawScore;
        //result->zScore = rawScore;
        result->zScore = (((float)rawScore) - mu)/ sqrtMu;
        elementCounter++;
    }

    unsigned int   seqIdPrev = counterOutput[0].id;
    unsigned short scoreMax =  counterOutput[0].count;
    //  std::cout << seqIdPrev << " " << (int) counterOutput[0].count << std::endl;
    if(resultSize == 1){
        hit_t *result = (resList + elementCounter);
        result->seqId = seqIdPrev;
        result->prefScore = scoreMax;
        result->zScore = (((float)scoreMax) - mu)/ sqrtMu;

        elementCounter++;
    }
    for (size_t i = 1; i < resultSize; i++) {
        const unsigned int seqIdCurr = counterOutput[i].id;
        const unsigned int scoreCurr = counterOutput[i].count;

        // if new sequence occurs or end of data write the result back
        if (seqIdCurr != seqIdPrev || i == (resultSize - 1)) {
            // write result to list
            if(scoreMax > thr && id != seqIdPrev){
                scoreMax += (seqIdCurr != seqIdPrev ) ? 0 : scoreCurr;
                hit_t *result = (resList + elementCounter);
                result->seqId = seqIdPrev;
                result->prefScore = scoreMax;
                result->zScore = (((float)scoreMax) - mu)/ sqrtMu;

                elementCounter++;
                if (elementCounter >= QueryScore::MAX_RES_LIST_LEN)
                    break;
            }
            seqIdPrev = seqIdCurr;
            // reset values
            scoreMax = 0;  // current best score
        }
        scoreMax += scoreCurr;

    }

    if(elementCounter > 1){
        if (id != UINT_MAX){
            std::sort(resList + 1, resList + elementCounter, QueryScore::compareHits);
        }
        else{
            std::sort(resList, resList + elementCounter, QueryScore::compareHits);
        }
    }
    std::pair<hit_t *, size_t>  pair = std::make_pair(this->resList, elementCounter);
    return pair;
}

