extern "C" {
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include "inttypes.h"

#include "utilities.h"
#include "clparsing.h"
#include "kmer.h"
#include "fastq_seq.h"
#include "bloom_filter.h"
}

#include "sparse_word_hash.h"

char* program_version       = "";
char* program_name          = "merge_STR_reads";
char* program_description   = 
    "Merge reads that support the same STR";
char* program_use           = 
    "merge_STR_reads [options] klength reads.str.fq";

Bool debug_flag;

// A percent identity threshold. 
const double pid_threshold = 90.0;

static void Reverse(char* const seq, const uint len)
{
    char* s = seq;
    char* p = seq + len - 1;
    
    while (s <= p) {
        uchar c;
        c = *s;
        *s = *p;
        *p = c;
        ++s; --p;
    }
}

// align the two sequences, and calculate the consensus sequence
static void Align(const char* const seq1, const char* const qual1, 
                  const uint zstart1, const uint end1,
                  const char* const seq2, const char* const qual2,
                  const uint zstart2, const uint end2,
                  uint* gaps, float* pid, char** pseq, char** pqual, 
                  const uint slen, const Bool is_right_gapped)
{
    char* seq = *pseq;
    char* qual = *pqual;

    uint len1 = end1 - zstart1 + 1;
    uint len2 = end2 - zstart2 + 1;

    const char* t1 = seq1 + zstart1;
    const char* q1 = qual1 + zstart1;
    const char* t2 = seq2 + zstart2;
    const char* q2 = qual2 + zstart2;

    // counters in loops
    uint i, j;

    // initialize the scores
    int match    = 1;
    int mismatch = -1;
    int gap  = -3;

    int** A = (int**)CkalloczOrDie(len1 * sizeof(int*));
    for(i = 0; i < len1; i++) {
        A[i] = (int*)CkalloczOrDie(len2 * sizeof(int));
    }
    int** B = (int**)CkalloczOrDie(len1 * sizeof(int*));
    for(i = 0; i < len1; i++) {
        B[i] = (int*)CkalloczOrDie(len2 * sizeof(int));
    }

    int best = 0, optlox = 0, optloy = 0;
    int score1, score2, score3;

    for (i = 1; i < len1; i++) {
        for (j = 1; j < len2; j++) {
            score1 = A[i][j-1] + gap;

            score2 = A[i-1][j] + gap;

            if (t1[i-1] == t2[j-1]) {
                score3 = A[i-1][j-1] + match;
            } else {
                score3 = A[i-1][j-1] + mismatch;
            }

            A[i][j] = MAX(MAX(score1,score2),MAX(score3,0));

            if (A[i][j] == score3) {
                B[i][j] = 0;
            } else if (A[i][j] == score2) {
                B[i][j] = 1;
            } else if (A[i][j] == score1) {
                B[i][j] = 2;
            }
    
            if (A[i][j] >= best) {
                best = A[i][j];
                optlox = i;
                optloy = j;
            }
        }
    }

    // trace backwards to find the best location 
    int matches = 0, mismatches = 0;
    int num_gaps = 0;

    i = optlox;
    j = optloy;
    int max_score = best;
    int sindx = 0;

    if (is_right_gapped) {
        uint a = 0;
        if (len1 > len2) {
            int a = len1-1;
            while (a > j) {
                seq[sindx] = t1[a-1];
                qual[sindx++] = q1[a-1];
                a--;
            }
        } else {
            int a = len2-1;
            while (a > j) {
                seq[sindx] = t2[a-1];
                qual[sindx++] = q2[a-1];
                a--;
            }
        }
    } else {
        num_gaps = len2 - optloy - 1;
    }

    while ((max_score > 0) && (i >= 1) && (j >= 1)) {
        if (B[i][j] == 0) {
            if (q1[i-1] > q2[j-1]) {
                seq[sindx] = t1[i-1];
                qual[sindx] = q1[i-1];
            } else {
                seq[sindx] = t2[j-1];
                qual[sindx] = q2[j-1];
            }
            sindx++;
            if (t1[i-1] != t2[j-1]) {
                mismatches++;
            } else {
                matches++;
            }
            i--; j--;
        } else if (B[i][j] == 1) {
            if (q1[i-1] > '5') {
                seq[sindx] = t1[i-1];
                qual[sindx++] = q1[i-1];
            }
            i--;
            num_gaps++;
        } else if (B[i][j] == 2) {
            if (q2[j-1] > '5') {
                seq[sindx] = t2[j-1];
                qual[sindx++] = q2[j-1];
            }
            j--;
            num_gaps++;
        }
        
        max_score = A[i][j];
    }

    if (is_right_gapped) {
        num_gaps = MAX(i,j) - MIN(i,j);
    } else {
        uint a = 0;
        if (len1 > len2) {
            int a = i;
            while (a > 0) {
                seq[sindx] = t1[a-1];
                qual[sindx++] = q1[a-1];
                a--;
            }
        } else {
            int a = j;
            while (a > 0) {
                seq[sindx] = t2[a-1];
                qual[sindx++] = q2[a-1];
                a--;
            }
        }
    }

    Reverse(seq, sindx);
    Reverse(qual, sindx);
    seq[sindx] = 0;
    qual[sindx] = 0;

    *gaps = num_gaps;
    *pid = matches * 100.0 / (matches + mismatches);

    for(i = 0; i < len1; i++) {
        Ckfree(A[i]);
    }
    Ckfree(A);
    for(i = 0; i < len1; i++) {
        Ckfree(B[i]);
    }
    Ckfree(B);
}

static Bool AlignFlanks(Block* const block,
                        const FastqSequence* const seq,
                        const char* const motif,
                        const int copies,
                        const int zstart,
                        const int end,
                        const uint max_threshold)
{
    float pid;    
    uint gaps = 0;
    
    int slen = MAX(block->slen,seq->slen);
    
    // Align the sequences to the left of the STR
    char* lseq   = (char*)CkalloczOrDie(slen);
    char* lqual  = (char*)CkalloczOrDie(slen);

    Align(block->seq, block->qual, 0, block->zstart, 
    seq->bases, seq->quals, 0, zstart, &gaps, &pid, &lseq, &lqual, slen, FALSE);

    if ((pid < pid_threshold) || (gaps > 2)) {
        if (debug_flag) {
            PrintDebugMessage(
            "Low pid (%2.2f) or too many gaps (%d) for the left flank.", 
            pid, gaps);
        }
        Ckfree(lseq);
        Ckfree(lqual);
        return NULL;
    }

    // Align the sequences to the right of the STR
    char* rseq   = (char*)CkalloczOrDie(slen);
    char* rqual  = (char*)CkalloczOrDie(slen);

    Align(block->seq, block->qual, block->end, block->slen, 
    seq->bases, seq->quals, end, seq->slen, &gaps, &pid,&rseq,&rqual,slen, TRUE);

    if ((pid < pid_threshold) || (gaps > 2)) {
        if (debug_flag) {
            PrintDebugMessage(
            "Low pid (%2.2f) or too many gaps (%d) for the right flank.",
            pid, gaps);
        }
        Ckfree(lseq);
        Ckfree(lqual);
        Ckfree(rseq);
        Ckfree(rqual);
        return FALSE;
    }

    // If I am here then these this block and this read seem like they support
    // the same STR. Lets combine them to create a new block
    block->zstart = strlen(lseq);
    block->support++;
    Bool found = FALSE;
    for(Copies* iter = block->supports; iter; iter = iter->next) {
        if (iter->copies == copies) {
            iter->nsupport += 1;
            found = TRUE;
            // sprintf(iter->name2, "%s", seq->name);
            break;
        }
    }    
    if (found == FALSE) {
        Copies* tmp = (Copies*)CkalloczOrDie(sizeof(Copies));
        tmp->copies = copies;
        tmp->nsupport = 1;
        // sprintf(tmp->name1, "%s", seq->name);
        SllAddHead(&block->supports, tmp);
    }
  
    block->end = block->zstart + strlen(motif);
    block->slen = block->end + strlen(rseq);
    Ckfree(block->seq);
    block->seq = (char*)CkalloczOrDie(block->slen+1);
    memcpy(block->seq, lseq, block->zstart);
    sprintf(block->seq + block->zstart, "%s", motif);
    memcpy(block->seq + block->end, rseq, block->slen - block->end);
    Ckfree(block->qual);
    block->qual = (char*)CkalloczOrDie(block->slen+1);
    memcpy(block->qual, lqual, block->zstart);
    for (int i = 0; i < strlen(motif); i++) {
        sprintf(block->qual + block->zstart + i, "!");
    }
    memcpy(block->qual + block->end, rqual, block->slen - block->end);

    Ckfree(lseq);
    Ckfree(lqual);
    Ckfree(rseq);
    Ckfree(rqual);

    return TRUE;
}

static void MergeShortTandemRepeatReads(const uint klength, 
                                        const char* const fqname,
                                        char** const argv, 
                                        const uint nameidx, 
                                        const uint progress_chunk,
                                        const uint min_threshold,
                                        const uint max_threshold,
                                        const Bool include_all)
{
    uint64_t num_sequence_processed = 0;

    // we store the blocks here
    SparseWordHashMap blocks;

    // buffer to write the keys into
    char* buffer = (char*)CkalloczOrDie(1024);

    FastqSequence* sequence = ReadFastqSequence(fqname, FALSE, FALSE);
    
    char name[1024];
    char fmotif[7], rmotif[7];
    int fcopies, fzstart, fend, rcopies, rzstart, rend;
    char lflank[1024], rflank[1024];

    Block* block = NULL;
    Bool merged_block = FALSE;

    while (sequence) {
        if (debug_flag == TRUE) {
            PrintDebugMessage("Processing %s", sequence->name + 1);
        } else {
            // print progress
            num_sequence_processed += 1;
            if ((num_sequence_processed - 1) % progress_chunk == 0) {
                PrintDebugMessage("Processing read number %"PRIu64": %s",
                num_sequence_processed, sequence->name + 1);
            }
        }
        
        // parse the name of the read.
        if (sscanf(sequence->name,
                   "%s\t%s\t%d\t%d\t%d\t%s\t%d\t%d\t%d\n",
                   name, fmotif, &fcopies, &fzstart, &fend, 
                   rmotif, &rcopies, &rzstart, &rend) != 9) {
            PrintMessageThenDie("Error in parsing read name %s",sequence->name);
        }
        
        // Check if the sequence aligns to some reads that we have already
        // processed
        merged_block = FALSE;
          
        memcpy(lflank, sequence->bases + (fzstart - klength), klength);
        lflank[klength] = '\0';
        memcpy(rflank, sequence->bases + fend, klength);
        rflank[klength] = '\0';
        sprintf(buffer, "%s %s %s", fmotif, lflank, rflank);
        if (debug_flag) fprintf(stderr, "%s\n", buffer);

        Bool in_hash = CheckInSparseWordHashMap(blocks, buffer);

        if (in_hash == TRUE) {
            block = blocks[buffer];
            for (Block* iter = block; iter; iter = iter->next) {
                merged_block = AlignFlanks(iter, sequence, fmotif, fcopies, 
                                           fzstart, fend, max_threshold);
                if (merged_block == TRUE) {
                    break;
                }
            }
        }

        if (merged_block == FALSE) {
            ReverseComplementSequence(sequence);
            memcpy(lflank, sequence->bases + (rzstart - klength), klength);
            lflank[klength] = '\0';
            memcpy(rflank, sequence->bases + rend, klength);
            rflank[klength] = '\0';
            sprintf(buffer, "%s %s %s", rmotif, lflank, rflank);
            if (debug_flag) fprintf(stderr, "%s\n", buffer);

            in_hash = CheckInSparseWordHashMap(blocks, buffer);
    
            if (in_hash == TRUE) {
                block = blocks[buffer];
                for (Block* iter = block; iter; iter = iter->next) {
                    merged_block = AlignFlanks(iter, sequence, rmotif, rcopies, 
                                               rzstart, rend, max_threshold);
                    if (merged_block == TRUE) {
                        break;
                    }
                }
            }
        }

        if (merged_block == FALSE) {
            block = (Block*)CkalloczOrDie(sizeof(Block));
            block->zstart = rzstart;
            block->end = rend;
            block->slen = sequence->slen;
            block->support = 1;
            block->seq = CopyString(sequence->bases);
            block->qual = CopyString(sequence->quals);
            block->supports = (Copies*)CkalloczOrDie(sizeof(Copies));
            block->supports->copies = rcopies;
            block->supports->nsupport = 1;
            // sprintf(block->supports->name1, "%s", sequence->name);

            if (in_hash == TRUE) {
                Block* head = blocks[buffer];
                SllAddHead(&head, block); 
            } else {
                blocks[CopyString(buffer)] = block;
            }
        }

        if (debug_flag) 
            fprintf(stderr, "-----------------------------------------------\n");
        sequence = GetNextSequence(sequence);
    }
    PrintDebugMessage("Processed %"PRIu64" reads.", num_sequence_processed);

    Ckfree(buffer);

    // lets print the merged blocks
    SparseWordHashMap::iterator it;
    uint bindex = 1;
    for (it = blocks.begin(); it != blocks.end(); it++) {
        if (sscanf((*it).first, "%s %*s %*s", fmotif) != 1) {
            PrintMessageThenDie("Error in parsing key : %s", (*it).first);
        }

        Block* block = (*it).second;

        for (Block* iter = block; iter; iter = iter->next) {
            int i=0;
            int maxcopies = 0;
            uint16_t copies[3];
            copies[0] = copies[1] = copies[2] = 0;
            uint16_t nsupport[3];
            nsupport[0] = nsupport[1] = nsupport[2] = 0;
            for (Copies* tmp = iter->supports; tmp; tmp=tmp->next) {
                if (tmp->nsupport >= 2) {
                    copies[i] = tmp->copies;
                    if (copies[i] > maxcopies) maxcopies = copies[i];
                    nsupport[i] = tmp->nsupport;
                    i++;
                }
                if (i == 3) break;
            }

            if ((iter->support >= min_threshold) && 
                (iter->support <= max_threshold)) {
                if ((!include_all && (copies[2] == 0) && (copies[1] != 0)) ||
                    ( include_all && (copies[2] == 0))) {
                    printf("@Block%d\t%s\t", bindex++, fmotif);
                    if (include_all && copies[1] == 0) {
                        printf("%d",copies[0]);
                    } else {
                        printf("%d,%d",copies[0], copies[1]);
                    }

                    ForceAssert(iter->end == (iter->zstart + strlen(fmotif)));
                    printf("\t%d\t%d\n", iter->zstart, 
                        iter->zstart + maxcopies * strlen(fmotif));

                    for (int j = 0; j < iter->zstart; j++) {
                        printf("%c", iter->seq[j]);
                    }
                    for (int j = 0; j < maxcopies; j++) {
                        printf("%s", fmotif);
                    }
                    for (int j = iter->end; j < iter->slen; j++) {
                        printf("%c", iter->seq[j]);
                    }
                    printf("\n");
                    printf("+\n");
                    for (int j = 0; j < iter->zstart; j++) {
                        printf("%c", iter->qual[j]);
                    }
                    for (int j = 0; j < (maxcopies * strlen(fmotif)); j++) {
                        printf("!");
                    }
                    for (int j = iter->end; j < iter->slen; j++) {
                        printf("%c", iter->qual[j]);
                    }
                    printf("\n");

                    // for (Copies* tmp = iter->supports; tmp; tmp=tmp->next) {
                    //    printf("%s\n", tmp->name1);
                    //    printf("%s\n", tmp->name2);
                    // }
                }
            }
            Ckfree(iter->seq);
            Ckfree(iter->qual);
            SllFreeList(&(iter->supports));
        }

        SllFreeList(&block);
    }
}


int main(int argc, char** argv) {
    // start time management
    t0 = time(0);

    // set the version number
    program_version = VERSION;

    // parse the command line
    CommandLineArguments* cl_options = NewCommandLineArguments();

    // these are the valid options for the various commands
    AddOption(&cl_options, "min_threshold", "4", TRUE, TRUE, 
    "Discard blocks that include < min_threshold reads", NULL);
    AddOption(&cl_options, "max_threshold", "10000", TRUE, TRUE,
    "Discard blocks that include > max_threshold reads", NULL);
    AddOption(&cl_options, "progress", "1000000", TRUE, TRUE,
    "print progress every so many sequences", NULL);
    AddOption(&cl_options, "all", "FALSE", FALSE, TRUE,
    "include non-polymorphic blocks", NULL);

    ParseOptions(&cl_options, &argc, &argv);

    // does the user just want some help
    Bool print_help = GetOptionBoolValueOrDie(cl_options, "help");
    if (print_help == TRUE) {
        PrintSimpleUsageString(cl_options);
        return EXIT_SUCCESS;
    }

    // does the user know what he/she is doing?
    if (argc < 2){
        PrintSimpleUsageString(cl_options);
        return EXIT_FAILURE;
    }

    uint kmer_length;
    if (sscanf(argv[1], "%u", &kmer_length) != 1) {
        #ifdef Large
        PrintMessageThenDie("Kmer length should be an odd integer < 64: %s",
        argv[1]);
        #else
        PrintMessageThenDie("Kmer length should be an odd integer < 32: %s",
        argv[1]);
        #endif
    }
    if (kmer_length % 2 == 0) {
        PrintWarning("Kmer length should be an odd integer, using %u",
        --kmer_length);
    }

    char* str_reads_name = argv[2];

    // kmers seen less than these many times should be ignored.
    uint min_threshold = GetOptionUintValueOrDie(cl_options, "min_threshold");
    uint max_threshold = GetOptionUintValueOrDie(cl_options, "max_threshold");
    Bool include_all   = GetOptionBoolValueOrDie(cl_options, "all");

    // do I need additional debug info
    debug_flag = GetOptionBoolValueOrDie(cl_options, "debug");

    // how often should I print progress?
    uint progress_chunk = GetOptionUintValueOrDie(cl_options, "progress");

    MergeShortTandemRepeatReads(kmer_length, 
                                str_reads_name,
                                argv, 
                                argc, 
                                progress_chunk,
                                min_threshold,
                                max_threshold,
                                include_all);

    FreeParseOptions(&cl_options, &argv);      
    return EXIT_SUCCESS;
}

