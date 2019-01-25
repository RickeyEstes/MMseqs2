#include <SubstitutionMatrixProfileStates.h>
#include <QueryMatcher.h>
#include "Alignment.h"
#include "Util.h"
#include "Debug.h"

#include "Matcher.h"
#include "DBWriter.h"
#include "NucleotideMatrix.h"
#include "SubstitutionMatrix.h"
#include "PrefilteringIndexReader.h"
#include "FileUtil.h"
#include "LinsearchIndexReader.h"
#include "IndexReader.h"


#ifdef OPENMP
#include <omp.h>
#endif

Alignment::Alignment(const std::string &querySeqDB,
                     const std::string &targetSeqDB,
                     const std::string &prefDB, const std::string &prefDBIndex,
                     const std::string &outDB, const std::string &outDBIndex,
                     const Parameters &par) :

        covThr(par.covThr), canCovThr(par.covThr), covMode(par.covMode), seqIdMode(par.seqIdMode), evalThr(par.evalThr), seqIdThr(par.seqIdThr),
        alnLenThr(par.alnLenThr), includeIdentity(par.includeIdentity), addBacktrace(par.addBacktrace), realign(par.realign), scoreBias(par.scoreBias),
        threads(static_cast<unsigned int>(par.threads)), compressed(par.compressed), outDB(outDB), outDBIndex(outDBIndex),
        maxSeqLen(par.maxSeqLen), compBiasCorrection(par.compBiasCorrection), altAlignment(par.altAlignment), qdbr(NULL), qDbrIdx(NULL),
        tdbr(NULL), tDbrIdx(NULL) {


    unsigned int alignmentMode = par.alignmentMode;
    if (alignmentMode == Parameters::ALIGNMENT_MODE_UNGAPPED) {
        Debug(Debug::ERROR) << "Use rescorediagonal for ungapped alignment mode.\n";
        EXIT(EXIT_FAILURE);
    }

    if (addBacktrace == true) {
        alignmentMode = Parameters::ALIGNMENT_MODE_SCORE_COV_SEQID;
    }

    if (realign == true) {
        alignmentMode = Parameters::ALIGNMENT_MODE_SCORE_ONLY;
        realignCov = par.covThr;
        covThr = 0.0;
        if (addBacktrace == false) {
            Debug(Debug::WARNING) << "Turn on backtrace for realign.\n";
            addBacktrace = true;
        }
    }

    std::string scoringMatrixFile = par.scoringMatrixFile;
    bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    tDbrIdx = new IndexReader(targetSeqDB, par.threads, IndexReader::SEQUENCES, touch);
    tdbr = tDbrIdx->sequenceReader;
    targetSeqType = tDbrIdx->getDbtype();
    sameQTDB = (targetSeqDB.compare(querySeqDB) == 0);
    if (sameQTDB == true) {
        qDbrIdx = tDbrIdx;
        qdbr = tdbr;
        querySeqType = targetSeqType;
    } else {
        // open the sequence, prefiltering and output databases
        qDbrIdx = new IndexReader(par.db1, par.threads,  IndexReader::SEQUENCES , false);
        qdbr = qDbrIdx->sequenceReader;
        querySeqType = qdbr->getDbtype();
    }

    if (altAlignment > 0) {
        if(Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)){
            Debug(Debug::ERROR) << "Alternative alignments are not supported for nucleotides.\n";
            EXIT(EXIT_FAILURE);
        }
//        if(realign==true){
//            Debug(Debug::ERROR) << "Alternative alignments do not supported realignment.\n";
//            EXIT(EXIT_FAILURE);
//        }
        alignmentMode = (alignmentMode > Parameters::ALIGNMENT_MODE_SCORE_COV) ? alignmentMode : Parameters::ALIGNMENT_MODE_SCORE_COV;
    }
    initSWMode(alignmentMode);

    //qdbr->readMmapedDataInMemory();
    // make sure to touch target after query, so if there is not enough memory for the query, at least the targets
    // might have had enough space left to be residung in the page cache
    if (sameQTDB == false && tDbrIdx == NULL && par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        tdbr->readMmapedDataInMemory();
    }

    if (qdbr->getSize() <= threads) {
        threads = qdbr->getSize();
    }

    if (querySeqType == -1 || targetSeqType == -1) {
        Debug(Debug::ERROR) << "Please recreate your database or add a .dbtype file to your sequence/profile database.\n";
        EXIT(EXIT_FAILURE);
    }
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE) && Parameters::isEqualDbtype(targetSeqType, Parameters::DBTYPE_HMM_PROFILE)) {
        Debug(Debug::ERROR) << "Only the query OR the target database can be a profile database.\n";
        EXIT(EXIT_FAILURE);
    }
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE) == false && Parameters::isEqualDbtype(targetSeqType, Parameters::DBTYPE_PROFILE_STATE_SEQ)) {
        Debug(Debug::ERROR) << "The query has to be a profile when using a target profile state database.\n";
        EXIT(EXIT_FAILURE);
    } else if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE) && Parameters::isEqualDbtype(targetSeqType, Parameters::DBTYPE_PROFILE_STATE_SEQ)) {
        querySeqType = Parameters::DBTYPE_PROFILE_STATE_PROFILE;
    }
    Debug(Debug::INFO) << "Query database type: " << DBReader<unsigned int>::getDbTypeName(querySeqType) << "\n";
    Debug(Debug::INFO) << "Target database type: " << DBReader<unsigned int>::getDbTypeName(targetSeqType) << "\n";

    prefdbr = new DBReader<unsigned int>(prefDB.c_str(), prefDBIndex.c_str(), threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
    prefdbr->open(DBReader<unsigned int>::LINEAR_ACCCESS);
    reversePrefilterResult = (Parameters::isEqualDbtype(prefdbr->getDbtype(), Parameters::DBTYPE_PREFILTER_REV_RES));

    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        m = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, scoreBias);
        gapOpen = 5;
        gapExtend = 2;
    } else if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_PROFILE_STATE_PROFILE)){
        SubstitutionMatrix s(par.scoringMatrixFile.c_str(), 2.0, scoreBias);
        this->m = new SubstitutionMatrixProfileStates(s.matrixName, s.probMatrix, s.pBack, s.subMatrixPseudoCounts, 2.0, scoreBias, 255);
        gapOpen = par.gapOpen;
        gapExtend = par.gapExtend;
    } else {
        // keep score bias at 0.0 (improved ROC)
        m = new SubstitutionMatrix(scoringMatrixFile.c_str(), 2.0, scoreBias);
        gapOpen = par.gapOpen;
        gapExtend = par.gapExtend;
    }

    if (realign == true) {
        realign_m = new SubstitutionMatrix(scoringMatrixFile.c_str(), 2.0, scoreBias-0.2f);
    } else {
        realign_m = NULL;
    }
}

void Alignment::initSWMode(unsigned int alignmentMode) {
    switch (alignmentMode) {
        case Parameters::ALIGNMENT_MODE_FAST_AUTO:
            if(covThr > 0.0 && seqIdThr == 0.0) {
                swMode = Matcher::SCORE_COV; // fast
            } else if(covThr > 0.0  && seqIdThr > 0.0) { // if seq id is needed
                swMode = Matcher::SCORE_COV_SEQID; // slowest
            } else {
                swMode = Matcher::SCORE_ONLY;
            }
            break;
        case Parameters::ALIGNMENT_MODE_SCORE_COV:
            swMode = Matcher::SCORE_COV; // fast
            break;
        case Parameters::ALIGNMENT_MODE_SCORE_COV_SEQID:
            swMode = Matcher::SCORE_COV_SEQID; // slowest
            break;
        default:
            swMode = Matcher::SCORE_ONLY;
            break;
    }

    // print out mode and check for errors
    switch (swMode) {
        case Matcher::SCORE_ONLY:
            Debug(Debug::INFO) << "Compute score only.\n";
            break;
        case Matcher::SCORE_COV:
            Debug(Debug::INFO) << "Compute score and coverage.\n";
            break;
        case Matcher::SCORE_COV_SEQID:
            Debug(Debug::INFO) << "Compute score, coverage and sequence id.\n";
            break;
        default:
            Debug(Debug::ERROR) << "Wrong swMode mode.\n";
            EXIT(EXIT_FAILURE);
    }
}

Alignment::~Alignment() {
    if (realign == true) {
        delete realign_m;
    }
    delete m;

    if (tDbrIdx != NULL) {
        delete tDbrIdx;
    }else{
        tdbr->close();
        delete tdbr;
    }

    if (sameQTDB == false) {
        if(qDbrIdx != NULL){
            delete qDbrIdx;
        }else{
            qdbr->close();
            delete qdbr;
        }
    }

    prefdbr->close();
    delete prefdbr;
}

void Alignment::run(const unsigned int mpiRank, const unsigned int mpiNumProc,
                    const unsigned int maxAlnNum, const unsigned int maxRejected) {

    size_t dbFrom = 0;
    size_t dbSize = 0;
    Util::decomposeDomainByAminoAcid(prefdbr->getAminoAcidDBSize(), prefdbr->getSeqLens(),
                                     prefdbr->getSize(), mpiRank, mpiNumProc, &dbFrom, &dbSize);

    Debug(Debug::INFO) << "Compute split from " << dbFrom << " to " << (dbFrom + dbSize) << "\n";
    std::pair<std::string, std::string> tmpOutput = Util::createTmpFileNames(outDB, outDBIndex, mpiRank);
    run(tmpOutput.first, tmpOutput.second, dbFrom, dbSize, maxAlnNum, maxRejected);

#ifdef HAVE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    if (MMseqsMPI::isMaster()) {
        std::vector<std::pair<std::string, std::string> > splitFiles;
        for (unsigned int proc = 0; proc < mpiNumProc; proc++) {
            splitFiles.push_back(Util::createTmpFileNames(outDB, outDBIndex, proc));
        }

        // merge output databases
        DBWriter::mergeResults(outDB, outDBIndex, splitFiles);
    }
}

void Alignment::run(const unsigned int maxAlnNum, const unsigned int maxRejected) {
    run(outDB, outDBIndex, 0, prefdbr->getSize(), maxAlnNum, maxRejected);
}

void Alignment::run(const std::string &outDB, const std::string &outDBIndex,
                    const size_t dbFrom, const size_t dbSize,
                    const unsigned int maxAlnNum, const unsigned int maxRejected) {
    size_t alignmentsNum = 0;
    size_t totalPassedNum = 0;
    DBWriter dbw(outDB.c_str(), outDBIndex.c_str(), threads, compressed, Parameters::DBTYPE_ALIGNMENT_RES);
    dbw.open();

    EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), this->m, gapOpen, gapExtend);
    size_t totalMemory = Util::getTotalSystemMemory();
    size_t flushSize = 1000000;
    if(totalMemory > prefdbr->getDataSize()){
        flushSize = dbSize;
    }

#pragma omp parallel num_threads(threads)
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
        std::string alnResultsOutString;
        alnResultsOutString.reserve(1024*1024);
        char buffer[1024+32768];
        Sequence qSeq(maxSeqLen, querySeqType, m, 0, false, compBiasCorrection);
        Sequence dbSeq(maxSeqLen, targetSeqType, m, 0, false, compBiasCorrection);
        Matcher matcher(querySeqType, maxSeqLen, m, &evaluer, compBiasCorrection, gapOpen, gapExtend);
        Matcher *realigner = NULL;
        if (realign ==  true) {
            realigner = new Matcher(querySeqType, maxSeqLen, realign_m, &evaluer, compBiasCorrection, gapOpen, gapExtend);
        }

        size_t iterations = static_cast<size_t>(ceil(static_cast<double>(dbSize) / static_cast<double>(flushSize)));
        for (size_t i = 0; i < iterations; i++) {
            size_t start = dbFrom + (i * flushSize);
            size_t bucketSize = std::min(dbSize - (i * flushSize), flushSize);

#pragma omp for schedule(dynamic, 5) reduction(+: alignmentsNum, totalPassedNum)
            for (size_t id = start; id < (start + bucketSize); id++) {
                Debug::printProgress(id);

                // get the prefiltering list
                char *data = prefdbr->getData(id, thread_idx);
                unsigned int queryDbKey = prefdbr->getDbKey(id);
                setQuerySequence(qSeq, id, queryDbKey, thread_idx);

                matcher.initQuery(&qSeq);
                // parse the prefiltering list and calculate a Smith-Waterman alignment for each sequence in the list
                std::vector<Matcher::result_t> swResults;
                std::vector<Matcher::result_t> swRealignResults;
                size_t passedNum = 0;
                unsigned int rejected = 0;

                while (*data != '\0' && passedNum < maxAlnNum && rejected < maxRejected) {
                    // DB key of the db sequence
                    char dbKeyBuffer[255 + 1];
                    const char* words[10];
                    Util::parseKey(data, dbKeyBuffer);
                    const unsigned int dbKey = (unsigned int) strtoul(dbKeyBuffer, NULL, 10);

                    size_t elements = Util::getWordsOfLine(data, words, 10);
                    int diagonal = INT_MAX;
                    bool isReverse = false;
                    // Prefilter result (need to make this better)
                    if(elements == 3){
                        hit_t hit = QueryMatcher::parsePrefilterHit(data);
                        isReverse = (reversePrefilterResult) ? hit.prefScore : false;
                        diagonal = hit.diagonal;
                    }

                    setTargetSequence(dbSeq, dbKey, thread_idx);
                    // check if the sequences could pass the coverage threshold
                    if(Util::canBeCovered(canCovThr, covMode, static_cast<float>(qSeq.L), static_cast<float>(dbSeq.L)) == false )
                    {
                        rejected++;
                        data = Util::skipLine(data);
                        continue;
                    }
                    const bool isIdentity = (queryDbKey == dbKey && (includeIdentity || sameQTDB)) ? true : false;

                    // calculate Smith-Waterman alignment
                    Matcher::result_t res = matcher.getSWResult(&dbSeq, diagonal, isReverse, covMode, covThr, evalThr, swMode, seqIdMode, isIdentity);
                    alignmentsNum++;

                    //set coverage and seqid if identity
                    if (isIdentity) {
                        res.qcov = 1.0f;
                        res.dbcov = 1.0f;
                        res.seqId = 1.0f;
                    }
                    if(checkCriteria(res, isIdentity, evalThr, seqIdThr, alnLenThr, covMode, covThr)){
                        swResults.emplace_back(res);
                        passedNum++;
                        totalPassedNum++;
                        rejected = 0;
                    }else{
                        rejected++;
                    }

                    data = Util::skipLine(data);
                }
                if(altAlignment > 0 && realign == false ){
                    computeAlternativeAlignment(queryDbKey, dbSeq, swResults, matcher, evalThr, swMode, thread_idx);
                }

                // write the results
                std::sort(swResults.begin(), swResults.end(), Matcher::compareHits);
                if (realign == true) {
                    realigner->initQuery(&qSeq);
                    for (size_t result = 0; result < swResults.size(); result++) {
                        setTargetSequence(dbSeq, swResults[result].dbKey, thread_idx);
                        const bool isIdentity = (queryDbKey == swResults[result].dbKey && (includeIdentity || sameQTDB)) ? true : false;
                        Matcher::result_t res = realigner->getSWResult(&dbSeq, INT_MAX, false, covMode, covThr, FLT_MAX,
                                                                       Matcher::SCORE_COV_SEQID, seqIdMode, isIdentity);
                        const bool covOK = Util::hasCoverage(realignCov, covMode, res.qcov, res.dbcov);
                        if(covOK == true|| isIdentity){
                            swResults[result].backtrace  = res.backtrace;
                            swResults[result].qStartPos  = res.qStartPos;
                            swResults[result].qEndPos    = res.qEndPos;
                            swResults[result].dbStartPos = res.dbStartPos;
                            swResults[result].dbEndPos   = res.dbEndPos;
                            swResults[result].alnLength  = res.alnLength;
                            swResults[result].seqId      = res.seqId;
                            swResults[result].qcov       = res.qcov;
                            swResults[result].dbcov      = res.dbcov;
                            swRealignResults.push_back(swResults[result]);
                        }
                    }
                    swResults = swRealignResults;
                    if(altAlignment> 0 ){
                        computeAlternativeAlignment(queryDbKey, dbSeq, swResults, matcher, FLT_MAX, Matcher::SCORE_COV_SEQID, thread_idx);
                    }
                }

                // put the contents of the swResults list into a result DB
                for (size_t result = 0; result < swResults.size(); result++) {
                    size_t len = Matcher::resultToBuffer(buffer, swResults[result], addBacktrace);
                    alnResultsOutString.append(buffer, len);
                }
                dbw.writeData(alnResultsOutString.c_str(), alnResultsOutString.length(), qSeq.getDbKey(), thread_idx);
                alnResultsOutString.clear();
            }

#pragma omp barrier
            if (thread_idx == 0) {
                prefdbr->remapData();
            }
#pragma omp barrier
        }

        if (realign == true) {
            delete realigner;
        }
    }

    dbw.close();

    Debug(Debug::INFO) << "\nAll sequences processed.\n\n";
    Debug(Debug::INFO) << alignmentsNum << " alignments calculated.\n";
    Debug(Debug::INFO) << totalPassedNum << " sequence pairs passed the thresholds ("
                       << ((float) totalPassedNum / (float) alignmentsNum) << " of overall calculated).\n";

    size_t hits = totalPassedNum / dbSize;
    size_t hits_rest = totalPassedNum % dbSize;
    float hits_f = ((float) hits) + ((float) hits_rest) / (float) dbSize;
    Debug(Debug::INFO) << hits_f << " hits per query sequence.\n";
}

inline void Alignment::setQuerySequence(Sequence &seq, size_t id, unsigned int key, int thread_idx) {

    // map the query sequence
    char *querySeqData = qdbr->getDataByDBKey(key, thread_idx);
    if (querySeqData == NULL) {
#pragma omp critical
        {
            Debug(Debug::ERROR) << "ERROR: Query sequence " << key
                                << " is required in the prefiltering, "
                                << "but is not contained in the query sequence database!\n"
                                << "Please check your database.\n";
            EXIT(EXIT_FAILURE);
        }
    }

    seq.mapSequence(id, key, querySeqData);
}

inline void Alignment::setTargetSequence(Sequence &seq, unsigned int key, int thread_idx) {

    char *dbSeqData = tdbr->getDataByDBKey(key, thread_idx);
    if (dbSeqData == NULL) {
#pragma omp critical
        {
            Debug(Debug::ERROR) << "ERROR: Sequence " << key
                                << " is required in the prefiltering,"
                                << "but is not contained in the target sequence database!\n"
                                << "Please check your database.\n";
            EXIT(EXIT_FAILURE);
        }
    }
    seq.mapSequence(static_cast<size_t>(-1), key, dbSeqData);
}


size_t Alignment::estimateHDDMemoryConsumption(int dbSize, int maxSeqs) {
    return 2 * (dbSize * maxSeqs * 21 * 1.75);
}


bool Alignment::checkCriteria(Matcher::result_t &res, bool isIdentity, double evalThr, double seqIdThr, int alnLenThr, int covMode, float covThr) {
    const bool evalOk = (res.eval <= evalThr); // -e
    const bool seqIdOK = (res.seqId >= seqIdThr); // --min-seq-id
    const bool covOK = Util::hasCoverage(covThr, covMode, res.qcov, res.dbcov);
    const bool alnLenOK = Util::hasAlignmentLength(alnLenThr, res.alnLength);

    // check first if it is identity
    if (isIdentity
        ||
        // general accaptance criteria
        ( evalOk   &&
          seqIdOK  &&
          covOK    &&
          alnLenOK
        ))
    {
        return true;
    } else {
        return false;
    }
}

void Alignment::computeAlternativeAlignment(unsigned int queryDbKey, Sequence &dbSeq,
                                            std::vector<Matcher::result_t> &swResults,
                                            Matcher &matcher, float evalThr, int swMode, int thread_idx) {
    int xIndex = m->aa2int[static_cast<int>('X')];
    size_t firstItResSize = swResults.size();
    for(size_t i = 0; i < firstItResSize; i++) {
        const bool isIdentity = (queryDbKey == swResults[i].dbKey && (includeIdentity || sameQTDB))
                                ? true : false;
        if (isIdentity == true) {
            continue;
        }
        setTargetSequence(dbSeq, swResults[i].dbKey, thread_idx);
        for (int pos = swResults[i].dbStartPos; pos < swResults[i].dbEndPos; ++pos) {
            dbSeq.int_sequence[pos] = xIndex;
        }
        bool nextAlignment = true;
        for (int altAli = 0; altAli < altAlignment && nextAlignment; altAli++) {
            Matcher::result_t res = matcher.getSWResult(&dbSeq, INT_MAX, false, covMode, covThr, evalThr, swMode,
                                                        seqIdMode, isIdentity);
            nextAlignment = checkCriteria(res, isIdentity, evalThr, seqIdThr, alnLenThr, covMode, covThr);
            if (nextAlignment == true) {
                swResults.emplace_back(res);
                for (int pos = res.dbStartPos; pos < res.dbEndPos; pos++) {
                    dbSeq.int_sequence[pos] = xIndex;
                }
            }
        }
    }
}
