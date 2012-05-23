#include "munkres.h"
#include <iostream>
#include <fstream>
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <map>
#include <getopt.h>
#include <set>
#include <list>
#include <cmath>
#include <cctype>
#include "../libs/snap/cliques.h"
#include <unistd.h>
#include <fuzzy.h>
#include <cstdarg>
#include "Clonewise.h"
#include <omp.h>

static void CreateFeatures();

double maxWeight = 0.0;
std::set<std::string> featureExceptions;
bool approxFilename = true;
const char *distroString = "ubuntu";
OutputFormat_e outputFormat = CLONEWISE_OUTPUT_TEXT;
bool doCheckRelated = false;
bool useSSDeep = true;
bool useExtensions = true;
int verbose = 0;
std::map<std::string, float> idf;
std::set<std::string> featureSet;
std::set<std::string> ignoreFalsePositives;
unsigned int numPackages = 0;
bool allPackages = false;
std::map<std::string, std::list<std::string> > packages;
std::map<std::string, std::map<std::string, std::set<std::string> > > packagesSignatures;
std::set<std::string> extensions;
bool reportError = false;
bool useRelativePathForSignature;
std::map<std::string, std::string> normalFeatures;
std::map<std::string, std::set<std::string> > embeddedList;

static void
errorLog(const char *fmt, ...)
{
	if (reportError) {
		va_list ap;

		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
}

unsigned int
LongestCommonSubsequenceLength(const char *s, unsigned int m, const char *t, unsigned int n)
{
       double d[m + 1][n + 1];
       unsigned int i, j;
       double cost;
       double D;

       D = 0;
       for (i = 0; i <= m; i++) {
               d[i][0] = 0.0;
       }
       for (j = 0; j <= n; j++) {
               d[0][j] = 0.0;
       }
       for (j = 1; j <= n; j++) {
               for (i = 1; i <= m; i++) {
                       if (s[i - 1] == t[j - 1]) {
                               d[i][j] = d[i-1][j-1] + 1;
                       } else {
                               d[i][j] = max(d[i][j - 1], d[i-1][j]);
                       }
               }
       }
       return d[m][n];
}

static unsigned int
SmithWatermanDistance(const char *s, unsigned int m, const char *t, unsigned int n)
{
       double d[m + 1][2];
       unsigned int i, j;
       double cost;
       double D;

       D = 0;
       for (i = 0; i <= m; i++) {
               d[i][0] = 0.0;
       }
       d[0][0] = 0.0;
       for (j = 1; j <= n; j++) {
               d[0][j % 2] = 0.0;
               for (i = 1; i <= m; i++) {
                       const static double G = 1.0; // gap cost

                       if (s[i - 1] == t[j - 1])
                               cost =  1.0;
                       else {
                               cost =  0.0;
                       }
                       d[i][j % 2] = max(
                               0.0,
// start over
                               max(d[i - 1][(j - 1) % 2] + cost,
// substitution
                               max(d[i - 1][j % 2] - G,
// insertion
                               d[i][(j - 1) % 2] - G)));
// deletion
                       if (d[i][j % 2] > D)
                               D = d[i][j % 2];
               }
       }
       return (int)max(m, n) - D;
}

static unsigned int
LevenshteinDistance(const char *s, unsigned int m, const char *t, unsigned int n)
{
       unsigned int d[m + 1][2];
       unsigned int i, j;
       unsigned int cost;

       for (i = 0; i <= m; i++) {
               d[i][0] = i;
       }
       d[0][0] = 0;
       for (j = 1; j <= n; j++) {
               d[0][j % 2] = j;
               for (i = 1; i <= m; i++) {
                       if (s[i - 1] == t[j - 1])
                               cost = 0;
                       else
                               cost = 1;
                       d[i][j % 2] = min(min(
                               d[i - 1][j % 2] + 1,            // insertion
                               d[i][(j - 1) % 2] + 1),         // deletion
                               d[i - 1][(j - 1) % 2] + cost);  // substitution
               }
       }
       return d[m][n % 2];
}

static void
LoadEmbeddedCodeCopiesList(const char *filename)
{
        std::ifstream stream;
        char s[1024];

        stream.open(filename);
        if (!stream) {
                fprintf(stderr, "Can't open embedded code copies list\n");
                exit(1);
        }
        while (!stream.eof()) {
                std::string str, s1, s2;
                size_t n;

                stream.getline(s, sizeof(s));
                if (s[0] == 0)
                        break;
                str = std::string(s);
                n = str.find_first_of('/');
                s1 = str.substr(0, n);
                s2 = str.substr(n + 1);
                if (packagesSignatures.find(s1) == packagesSignatures.end())
                        continue;
                if (packagesSignatures.find(s2) == packagesSignatures.end())
                        continue;
                embeddedList[s1].insert(s2);
        }
        stream.close();
}

static void
normalizeFeature(std::string &normalFeature, const std::string &feature)
{
	size_t lastDot;
	int i;

	lastDot = feature.find_last_of('.');
	i = 0;
	if (feature.size() >= 4 && feature[0] == 'l' && feature[1] == 'i' && feature[2] == 'b') {
		i = 3;
	}
	for (; i < feature.size(); i++) {
		if (isdigit(feature[i]) || feature[i] == '_' || feature[i] == '-' || (i != lastDot && feature[i] == '.'))
			continue;
		normalFeature += tolower(feature[i]);
	}
	if (normalFeature.size() > 4 && !strcmp(&normalFeature.c_str()[normalFeature.size() - 4], ".cxx")) {
		normalFeature = normalFeature.substr(0, normalFeature.size() - 2);
	} else if (normalFeature.size() > 4 && !strcmp(&normalFeature.c_str()[normalFeature.size() - 4], ".cpp")) {
		normalFeature = normalFeature.substr(0, normalFeature.size() - 2);
	} else if (normalFeature.size() > 3 && !strcmp(&normalFeature.c_str()[normalFeature.size() - 3], ".cc")) {
		normalFeature = normalFeature.substr(0, normalFeature.size() - 1);
	}
	if (normalFeature.size() == 0) {
		normalFeature = feature;
	}
}

static void
lineToFeature(const char *s, std::string &feature, std::string &hash)
{
	std::string str, str2, origFeature;
	size_t n, n1, n2;

	str = std::string(s);
	n2 = str.find_first_of(',');
	hash = str.substr(0, n2 - 1);
	n1 = str.find_first_of('"');
	n2 = str.find_last_of('"');
	str2 = str.substr(n1 + 1, n2 - n1 - 1);
	n = str2.find_last_of('/');
	origFeature = str2.substr(n + 1, str2.size() - n - 1);
	normalizeFeature(feature, origFeature);
}

static void
loadFeatureExceptions()
{
	std::ifstream stream;
	char s[1024];

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/feature-exceptions", distroString);
	stream.open(s);
	if (!stream) {
		fprintf(stderr, "Couldn't open feature-exceptions\n");
		exit(1);
	}
	while (!stream.eof()) {
		std::string str, feature;

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		str = std::string(s);
		normalizeFeature(str, feature);
		featureExceptions.insert(feature);
	}
	stream.close();
}

static void
BuildDatabase()
{
	if (getuid() != 0) {
		fprintf(stderr, "Need to be root to build database.\n");
		exit(1);
	}
	fprintf(stderr, "Building database. This could take a considerable amount of time..\n");
	system("Clonewise-BuildDatabase");
}

static void
BuildFeatures()
{
	if (getuid() != 0) {
		fprintf(stderr, "Need to be root to build database.\n");
		exit(1);
	}
	CreateFeatures();
}

bool
approxMatchFilenames(const std::string &x, const std::string &y)
{
	int xi, yi;
	unsigned int d;
	float s, s_min, s_max;
	int m_min, m_max;

	xi = x.size();
	yi = y.size();
	m_max = max((double)x.size(), (double)y.size());
	m_min = min((double)x.size(), (double)y.size());
	//d = SmithWatermanDistance(x.c_str(), x.size(), y.c_str(), y.size());
	d = LevenshteinDistance(x.c_str(), x.size(), y.c_str(), y.size());
//	d = LongestCommonSubsequenceLength(x.c_str(), x.size(), y.c_str(), y.size());
	s = 1.0 - (double)d/(double)m_max;
//	s_min = (double)d/(double)m_min;
//	s_max = (double)d/(double)m_max;
//	if (xi >= 5 && yi >= 5 && s_min >= 0.85 && s_max >= 0.65) {
	if (s >= 0.85) {
		return true;
	}
	while (xi > 0 && yi > 0 && x[xi - 1] == y[yi - 1]) {
		xi--;
		yi--;
	}
	if (xi >= 5 && yi >= 5) {
		return xi == 0 || yi == 0;
	} else {
		return xi == 0 && yi == 0;
	}
}

std::map<std::string, std::set<std::string> >::const_iterator
findFilenameFromPackage(const std::string &filename, const std::map<std::string, std::set<std::string> > & package, float &weight)
{
	std::map<std::string, std::set<std::string> >::const_iterator pIter;

	if (!approxFilename) {
		pIter = package.find(filename);
		weight = idf[filename];
		return pIter;
	} else {
		for (	pIter  = package.begin();
			pIter != package.end();
			pIter++)
		{
			if (approxMatchFilenames(filename, pIter->first)) {
//				weight = min((double)idf[filename], (double)idf[pIter->first]);
				weight = (double)idf[pIter->first];
				return pIter;
			}
		}
	}
	return package.end();
}

bool
IsProgramFilename(const std::string &feature)
{
	std::string ext;
	size_t n;

	n = feature.find_last_of('.');
	ext = feature.substr(n + 1);
	for (int i = 0; i < ext.size(); i++) {
		ext[i] = tolower(ext[i]);
	}
	return extensions.find(ext) != extensions.end();
}

void
printMatch(const std::map<std::string, std::set<std::string> > &embedding, const std::map<std::string, std::set<std::string> > &package, std::map<std::string, std::pair<std::string, float> > &matches)
{
	std::map<std::string, std::pair<std::string, float> >::iterator matchesIter;
	if (verbose >= 2) {
		for (	matchesIter  = matches.begin();
			matchesIter != matches.end();
			matchesIter++)
		{
			if (IsProgramFilename(matchesIter->first) || IsProgramFilename(matchesIter->second.first)) {
				if (outputFormat == CLONEWISE_OUTPUT_XML) {
					printf("\t\t<Match>\n");
					printf("\t\t\t<Filename1>%s</Filename1>\n", matchesIter->first.c_str());
					printf("\t\t\t<Filename2>%s</Filename2>\n", matchesIter->second.first.c_str());
					printf("\t\t\t<Weight>%f</Weight>\n", matchesIter->second.second);
					printf("\t\t</Match>\n");
				} else {
					printf("\t\tMATCH %s/%s (%f)\n", matchesIter->first.c_str(), matchesIter->second.first.c_str(), matchesIter->second.second);
				}
			}
		}
	}
}

bool
WriteCheckForClone(std::ofstream &testStream, const std::map<std::string, std::set<std::string> > &embedding, const std::map<std::string, std::set<std::string> > &package, const std::string &cl, std::map<std::string, std::pair<std::string, float> > &matches)
{
	std::map<std::string, std::set<std::string> >::const_iterator eIter;
	int found, foundFilenameHash, foundFilenameHash80, foundExactFilenameHash;
	int foundData, foundDataFilenameHash, foundDataFilenameHash80, foundDataExactFilenameHash;
	int foundHash, foundExactHash, foundDataExactHash;
	float s;
	float scoreFilename, scoreFilenameHash, scoreFilenameHash80, scoreExactFilenameHash;
	float scoreDataFilename, scoreDataFilenameHash, scoreDataFilenameHash80, scoreDataExactFilenameHash;
	float featureVector[NFEATURES];
	std::map<std::string, std::map<std::string, float> > possibleMatches;
	std::map<std::string, std::map<std::string, float> >::const_iterator possibleMatchesIter;
	std::set<std::string> possibleMatches2;

	foundDataExactHash = 0;
	foundExactHash = 0;
	foundHash = 0;

	found = 0;
	foundFilenameHash80 = 0;
	foundFilenameHash = 0;
	foundExactFilenameHash = 0;
	scoreFilenameHash = 0.0;
	scoreFilenameHash80 = 0.0;
	scoreFilename = 0.0;
	scoreExactFilenameHash = 0.0;

	foundData = 0;
	foundDataFilenameHash80 = 0;
	foundDataFilenameHash = 0;
	foundDataExactFilenameHash = 0;
	scoreDataFilenameHash = 0.0;
	scoreDataFilenameHash80 = 0.0;
	scoreDataFilename = 0.0;
	scoreDataExactFilenameHash = 0.0;

	for (	eIter  = embedding.begin();
		eIter != embedding.end();
		eIter++)
	{

		std::map<std::string, std::set<std::string> >::const_iterator pIter;
		float maxS;
		std::set<std::string>::const_iterator l1Iter;
		float weight;
		bool isData;

		isData = !IsProgramFilename(eIter->first);

#if 0
		for (	l1Iter  = eIter->second.begin();
			l1Iter != eIter->second.end();
			l1Iter++)
		{
			maxS = 0.0;
			for (	pIter  = package.begin();
				pIter != package.end();
				pIter++)
			{
				std::set<std::string>::const_iterator l2Iter;

				for (	l2Iter  = pIter->second.begin();
					l2Iter != pIter->second.end();
					l2Iter++)
				{
					float s;

					s = fuzzy_compare(l1Iter->c_str(), l2Iter->c_str());
					if (s > maxS) {
						maxS = s;
						goto skip1;
					}
				}
			}
skip1:
			if (maxS > 0.0) {
				foundHash++;
			}
		}
#endif

		for (	l1Iter  = eIter->second.begin();
			l1Iter != eIter->second.end();
			l1Iter++)
		{
			for (	pIter  = package.begin();
				pIter != package.end();
				pIter++)
			{
				if (pIter->second.find(*l1Iter) != pIter->second.end()) {
					if (isData) {
						foundDataExactHash++;
					} else {
						foundExactHash++;
					}
					goto skip3;
				}
			}
		}
skip3:
		pIter = findFilenameFromPackage(eIter->first, package, weight);
		if (pIter != package.end()) {
			possibleMatches[eIter->first][pIter->first] = weight;
			possibleMatches2.insert(pIter->first);
			maxS = 0.00;
			for (	l1Iter  = eIter->second.begin();
				l1Iter != eIter->second.end();
				l1Iter++)
			{
				std::set<std::string>::const_iterator l2Iter;

				if (pIter->second.find(*l1Iter) != pIter->second.end()) {
					maxS = 100.0;
					if (isData) {
						foundDataExactFilenameHash++;
						scoreDataExactFilenameHash += weight;
					} else {
						foundExactFilenameHash++;
						scoreExactFilenameHash += weight;
					}
					goto skip2;
				}
				for (	l2Iter  = pIter->second.begin();
					l2Iter != pIter->second.end();
					l2Iter++)
				{
					float s;

					s = fuzzy_compare(l1Iter->c_str(), l2Iter->c_str());
					if (s > maxS) {
						maxS = s;
					}
				}
			}
skip2:
			if (maxS > 0.0) {
				if (isData) {
					foundDataFilenameHash++;
					scoreDataFilenameHash += weight;
				} else {
					foundFilenameHash++;
					scoreFilenameHash += weight;
				}
			}
			if (maxS > 80.0) {
				if (isData) {
					foundDataFilenameHash80++;
					scoreDataFilenameHash80 += weight;
				} else {
					foundFilenameHash80++;
					scoreFilenameHash80 += weight;
				}
			}
		}
	}

	Matrix<double> matrix(possibleMatches.size(), possibleMatches2.size());
	int i;

	i = 0;
	for (	possibleMatchesIter  = possibleMatches.begin();
		possibleMatchesIter != possibleMatches.end();
		possibleMatchesIter++, i++)
	{
		std::set<std::string>::const_iterator possibleMatchesIter2;
		int j;

		j = 0;
		for (	possibleMatchesIter2  = possibleMatches2.begin();
			possibleMatchesIter2 != possibleMatches2.end();
			possibleMatchesIter2++, j++)
		{
			matrix(i, j) = maxWeight - possibleMatches[possibleMatchesIter->first][*possibleMatchesIter2];
		}
	}
	Munkres m;
	m.solve(matrix);

	i = 0;
	for (	possibleMatchesIter  = possibleMatches.begin();
		possibleMatchesIter != possibleMatches.end();
		possibleMatchesIter++, i++)
	{
		std::set<std::string>::const_iterator possibleMatchesIter2;
		int j;

		j = 0;
		for (	possibleMatchesIter2  = possibleMatches2.begin();
			possibleMatchesIter2 != possibleMatches2.end();
			possibleMatchesIter2++, j++)
		{
			if (matrix(i, j) == 0) {
				bool isData;
				float weight;

				weight = possibleMatches[possibleMatchesIter->first][*possibleMatchesIter2];
				isData = !IsProgramFilename(possibleMatchesIter->first);
				if (isData) {
					foundData++;
					scoreDataFilename += weight;
				} else {
					found++;
					scoreFilename += weight;
				}
				matches[possibleMatchesIter->first] = std::pair<std::string, float>(*possibleMatchesIter2, weight);
				break;
			}
		}
	}
	featureVector[ 0] = embedding.size();
	featureVector[ 1] = package.size();

	featureVector[ 2] = found;
	featureVector[ 3] = foundFilenameHash;
	featureVector[ 4] = foundFilenameHash80;
	featureVector[ 5] = foundExactFilenameHash;
	featureVector[ 6] = scoreFilename;
	featureVector[ 7] = scoreFilenameHash;
	featureVector[ 8] = scoreFilenameHash80;
	featureVector[ 9] = scoreExactFilenameHash;

	featureVector[10] = foundData;
	featureVector[11] = foundDataFilenameHash;
	featureVector[12] = foundDataFilenameHash80;
	featureVector[13] = foundDataExactFilenameHash;
	featureVector[14] = scoreDataFilename;
	featureVector[15] = scoreDataFilenameHash;
	featureVector[16] = scoreDataFilenameHash80;
	featureVector[17] = scoreDataExactFilenameHash;

	featureVector[18] = foundHash;
	featureVector[19] = foundExactHash;
	featureVector[20] = foundDataExactHash;

printf("# yop %i\n", found);
fflush(stdout);

	if (featureVector[2] == 0)
		return false;

	for (int i = 0; i < NFEATURES; i++) {
		testStream << featureVector[i] << ",";
	}
	testStream << cl << "\n";
//printMatch(embedding, package, matches);
	return true;
}

void
LoadSignature(std::string name, std::map<std::string, std::set<std::string> > &signature)
{
	std::ifstream stream;

	stream.open(name.c_str());
	if (!stream) {
		errorLog("Couldn't open %s\n", name.c_str());
	}
	while (!stream.eof()) {
		std::string str, str2, hash, feature;
		char s[1024];
		size_t n, n1, n2;

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		lineToFeature(s, feature, hash);
		if (featureExceptions.find(feature) == featureExceptions.end()) {
			signature[feature].insert(hash);
		}
	}
	stream.close();
}

void
printArffHeader(std::ofstream &testStream)
{
	testStream << "@RELATION Clones\n";

	testStream << "@ATTRIBUTE N_Filenames_A NUMERIC\n";
	testStream << "@ATTRIBUTE N_Filenames_B NUMERIC\n";

	testStream << "@ATTRIBUTE N_Common_Filenames NUMERIC\n";
	testStream << "@ATTRIBUTE N_Common_FilenameHashes NUMERIC\n";
	testStream << "@ATTRIBUTE N_Common_FilenameHash80 NUMERIC\n";
	testStream << "@ATTRIBUTE N_Common_ExactFilenameHash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Score_of_Common_Filename NUMERIC\n";
	testStream << "@ATTRIBUTE N_Score_of_Common_FilenameHash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Score_of_Common_FilenameHash80 NUMERIC\n";
	testStream << "@ATTRIBUTE N_Score_of_Common_ExactFilenameHash80 NUMERIC\n";

	testStream << "@ATTRIBUTE N_Data_Common_Filenames NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Common_FilenameHashes NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Common_FilenameHash80 NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Common_ExactFilenameHash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Score_of_Common_Filename NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Score_of_Common_FilenameHash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Score_of_Common_FilenameHash80 NUMERIC\n";
	testStream << "@ATTRIBUTE N_Data_Score_of_Common_ExactFilenameHash80 NUMERIC\n";

	testStream << "@ATTRIBUTE N_Common_Hash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Common_ExactHash NUMERIC\n";
	testStream << "@ATTRIBUTE N_Common_DataExactHash NUMERIC\n";

	testStream << "@ATTRIBUTE CLASS {Y,N}\n";
	testStream << "@DATA\n";
}

int
GetScoreForNotEmbedded(std::ofstream &testStream)
{
        std::map<std::string, std::list<std::string> >::const_iterator pIter;
        std::string p1, p2;
        int n1, n2, i;
        bool breakit, doit;
        float featureVector[NFEATURES];
	int skip;
	std::map<std::string, std::pair<std::string, float> > matches;

	skip = 0;
	do {
		doit = false;
		do {
        	        n1 = rand() % packages.size();
			n2 = rand() % packages.size();
			breakit = true;
			for (   pIter  = packages.begin(), i = 0;
                        	pIter != packages.end() && i < n1;
                        	pIter++, i++);
			p1 = pIter->first;
			for (   pIter  = packages.begin(), i = 0;
				pIter != packages.end() && i < n2;
				pIter++, i++);
			p2 = pIter->first;
			if (embeddedList.find(p1) != embeddedList.end() && embeddedList[p1].find(p2) != embeddedList[p1].end())
				breakit = false;
			else if (embeddedList.find(p2) != embeddedList.end() && embeddedList[p2].find(p1) != embeddedList[p2].end())
				breakit = false;
		} while (!breakit);
		if (!WriteCheckForClone(testStream, packagesSignatures[p1], packagesSignatures[p2], "N", matches)) {
			doit = true;
			skip++;
		}
	} while (doit);
	return 1 + skip;
}

int
DoScoresForEmbedded(std::ofstream &testStream)
{
	std::map<std::string, std::set<std::string> >::const_iterator iter1;
	std::map<std::string, std::pair<std::string, float> > matches;
	int total;
	int fp;

	fp = 0;
	total = 0;
        for (   iter1  = embeddedList.begin();
                iter1 != embeddedList.end();
                iter1++)
        {
                std::set<std::string>::const_iterator iter2;
                std::map<std::string, std::set<std::string> > *sig1, *sig2;

                sig1 = &packagesSignatures[iter1->first];
                for (   iter2  = iter1->second.begin();
                        iter2 != iter1->second.end();
                        iter2++, total++)
                {
                        sig2 = &packagesSignatures[*iter2];
			printf("# y0p %s %s\n", iter1->first.c_str(), iter2->c_str());
			if (!WriteCheckForClone(testStream, *sig1, *sig2, "Y", matches)) {
				fp++;
			}
                }
        }
	printf("# total positives %i (fp %i)\n", total, fp);
	return total - fp;
}

void
trainModel()
{
	std::ofstream testStream;
	char cmd[1024], s[1024], testFilename[1024];
	int c, total;

	srand(0);
	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/embedded-code-copies", distroString);
	LoadEmbeddedCodeCopiesList(s);

	snprintf(testFilename, sizeof(testFilename), "/tmp/train_%i.arff", rand());
	testStream.open(testFilename);
	if (!testStream) {
		fprintf(stderr, "Can't write test.arff\n");
		return;
	}
	printArffHeader(testStream);
	c = DoScoresForEmbedded(testStream);
	total = 0;
	c = 0;
	for (int i = 0; total < 50000; i++) {
		total += GetScoreForNotEmbedded(testStream);
		c++;
	}
	printf("# total negatives (non zero %i) out of %i\n", c, total);
	testStream.close();
	snprintf(cmd, sizeof(cmd), "java -cp /usr/share/java/weka.jar weka.classifiers.trees.RandomForest -I 10 -K 0 -S 1 -d /var/lib/Clonewise/distros/%s/model -t %s", distroString, testFilename);
	system(cmd);
	//unlink(testFilename);
}

static void
checkPackage(std::map<std::string, std::set<std::string > > &embedding, const char *name)
{
	std::map<std::string, std::map<std::string, std::pair<std::string, float> > > matchesTable;
	std::map<std::string, std::list<std::string> >::const_iterator pIter;
	float featureVector[NFEATURES];
	char cmd[1024], testFilename[1024], str[1024];
	FILE *p;
	std::ofstream testStream;
	std::set<std::string> skip;

	snprintf(testFilename, sizeof(testFilename), "/tmp/test_%i.arff", rand());
	testStream.open(testFilename);
	if (!testStream) {
		fprintf(stderr, "Can't write test.arff\n");
		return;
	}
	printArffHeader(testStream);

	for (	pIter  = packages.begin();
		pIter != packages.end();
		pIter++)
	{
		const std::map<std::string, std::set<std::string> > *package;
		std::string fp;

		fp = std::string(name) + std::string("/") + pIter->first;
		if (ignoreFalsePositives.find(fp) != ignoreFalsePositives.end())
			continue;

		package = &packagesSignatures[pIter->first];
		if (strcmp(name, pIter->first.c_str()) != 0) {
			if (!WriteCheckForClone(testStream, embedding, *package, "?", matchesTable[pIter->first]))
				skip.insert(fp);
		} else {
			skip.insert(fp);
		}
	}
	testStream.close();

	snprintf(cmd, sizeof(cmd), "java -cp /usr/share/java/weka.jar weka.classifiers.trees.RandomForest -l /var/lib/Clonewise/distros/%s/model -T %s -p 0", distroString, testFilename);
	p = popen(cmd, "r");
	if (p == NULL) {
		unlink(testFilename);
		fprintf(stderr, "Can't popen\n");
		return;
	}
	for (int i = 0; i < 5; i++) {
		fgets(str, sizeof(str), p);
	}
	for (
		pIter = packages.begin();
		!feof(p) && pIter != packages.end();
		pIter++)
	{
		const std::map<std::string, std::set<std::string> > *package;
		std::string fp;

		fp = std::string(name) + std::string("/") + pIter->first;
		if (ignoreFalsePositives.find(fp) != ignoreFalsePositives.end())
			continue;

		if (skip.find(fp) != skip.end())
			continue;

		package = &packagesSignatures[pIter->first];

		str[27] = 0;
		fgets(str, sizeof(str), p);
		if (str[27] == 'Y') {
			if (outputFormat == CLONEWISE_OUTPUT_XML) {
				printf("\t<Clone>\n");
				printf("\t\t<SourcePackage>%s</SourcePackage>\n", name);
				printf("\t\t<ClonedSourcePackage>%s</ClonedSourcePackage>\n", pIter->first.c_str());
			} else {
				printf("%s CLONED_IN_SOURCE %s\n", name, pIter->first.c_str());		
			}
			printMatch(embedding, *package, matchesTable[pIter->first]);
			if (verbose >= 1) {
				std::list<std::string>::const_iterator nIter;

				for (	nIter  = pIter->second.begin();
					nIter != pIter->second.end();
					nIter++)
				{
					if (outputFormat == CLONEWISE_OUTPUT_XML) {
						printf("\t\t<ClonedPackage>%s</ClonedPackage>\n", nIter->c_str());
					} else {
						printf("\t%s CLONED_IN_PACKAGE %s\n", name, nIter->c_str());
					}
				}
			}
			if (outputFormat == CLONEWISE_OUTPUT_XML) {
				printf("\t</Clone>\n");
			}
		}
	}
	pclose(p);
//	unlink(testFilename);
}

static void
checkRelated()
{
#if 0
	std::map<std::string, int> packagesIndex;
	std::map<int, std::string> packagesReverseIndex;
	int packagesCount = 1;
	PUNGraph g;
	std::ifstream stream;
	TVec<TIntV> CmtyV;
	const int OverlapSz = 2;
	std::map<std::string, std::list<std::string> >::const_iterator pIter;
	char s[1024];
	float featureVector[NFEATURES];

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/scores", distroString);
	stream.open(s);
	if (!stream) {
		std::ofstream scoresFile;
		std::map<std::string, std::list<std::string> >::const_iterator pIter1;

		if (getuid() != 0) {
			fprintf(stderr, "Need to be root to build database.\n");
			exit(1);
		}
		fprintf(stderr, "Building database. This could take a considerable amount of time..\n");
		snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/scores", distroString);
		scoresFile.open(s);
		if (!scoresFile) {
			fprintf(stderr, "Couldn't open scores\n");
			exit(1);
		}
		for (	pIter1  = packages.begin();
			pIter1 != packages.end();
			pIter1++)
		{
			std::map<std::string, std::list<std::string> >::const_iterator pIter2;

			for (	pIter2  = packages.begin();
				pIter2 != packages.end();
				pIter2++)
			{
				float score;

//				CheckForClone(packagesSignatures[pIter1->first], packagesSignatures[pIter2->first], score, featureVector);
				if (score >= 5.0) {
					scoresFile << "THRESHOLD" << "/" << pIter1->first.c_str() << "/" << pIter2->first.c_str() << "/" << score << "\n";
				}
			}
		}
		scoresFile.close();
		snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/scores", distroString);
		stream.open(s);
		if (!stream) {
			fprintf(stderr, "Couldn't open scores\n");
			exit(1);
		}
	}
	g = TUNGraph::New();
	for (	pIter  = packages.begin();
		pIter != packages.end();
		pIter++)
	{
		packagesIndex[pIter->first] = packagesCount;
		g->AddNode(packagesCount);
		packagesReverseIndex[packagesCount] = pIter->first;
		packagesCount++;
	}
	while (!stream.eof()) {
		std::string str, pkgEmbedded, pkgMain;
		char s[1024];
		float threshold;
		size_t n;

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		if (strncmp(s, "THRESHOLD/", sizeof("THRESHOLD/") - 1) != 0)
			continue;
		str = std::string(&s[sizeof("THRESHOLD/")] - 1);
		n = str.find_first_of('/');
		if (n == std::string::npos)
			continue;
		pkgEmbedded = str.substr(0, n);
		str = str.substr(n + 1);
		n = str.find_first_of('/');
		if (n == std::string::npos)
			continue;
		pkgMain = str.substr(0, n);
		str = str.substr(n + 1);
		threshold = strtof(str.c_str(), NULL);
		if (pkgMain.size() == 0 || pkgEmbedded.size() == 0)
			continue;
		if (packagesIndex.find(pkgMain) == packagesIndex.end() || packagesIndex.find(pkgEmbedded) == packagesIndex.end()) {
			continue;
		}
		if (threshold >= g_OverlapScoreMeasure_Threshold) {
			g->AddEdge(packagesIndex[pkgEmbedded], packagesIndex[pkgMain]);
		}
	}
	stream.close();
	//TCliqueOverlap::GetCPMCommunities(g, OverlapSz+1, CmtyV);
	TCliqueOverlap::GetMaxCliques(g, 3, CmtyV);
	printf("%d PACKAGE_GROUPS\n", CmtyV.Len());
	for (int i = 0; i < CmtyV.Len(); i++) {
		printf("RELATED\n");
		for (int j = 0; j < CmtyV[i].Len(); j++) {
			printf("\t%s\n", packagesReverseIndex[CmtyV[i][j].Val].c_str());
		}
	}
#endif
}

void
LoadPackagesInfo()
{
	std::ifstream stream;
	char s[1024];

	if (numPackages != 0)
		return;

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/packages", distroString);
	stream.open(s);
	if (!stream) {
		fprintf(stderr, "Couldn't open packages\n");
		exit(1);
	}
	while (!stream.eof()) {
		std::string str, sigName, srcName;
		size_t n;
		char s[1024];

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		str = std::string(s);
		n = str.find_last_of('/');
		if (n == std::string::npos)
			return;
		sigName = str.substr(0, n);
		srcName = str.substr(n + 1);
		if (sigName.size() > 0 && srcName.size() > 0) {
			packages[srcName].push_back(sigName);
		}
	}
	stream.close();
	numPackages = packages.size();
}

static void
ReadIgnoreList(const char *filename)
{
	std::ifstream stream;

	stream.open(filename);
	if (stream) {
		while (!stream.eof()) {
			char s[1024];

			stream.getline(s, sizeof(s));
			if (s[0] == 0)
				break;
			ignoreFalsePositives.insert(s);
		}
		stream.close();
	}
}

int
LoadEverything()
{
	std::string filename;
	std::ifstream stream;
	char s[1024];
	std::map<std::string, std::list<std::string> >::const_iterator pIter;
	std::map<std::string, float>::iterator idfIter;

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/.done", distroString);
	if (access(s, R_OK) != 0)
		BuildDatabase();

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/extensions", distroString);
	stream.open(s);
	if (!stream) {
		fprintf(stderr, "Couldn't open extensions\n");
		exit(1);
	}
	while (!stream.eof()) {
		std::string ext;

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		ext = std::string(s);
		for (int i = 0; i < ext.size(); i++) {
			ext[i] = tolower(ext[i]);
		}
		extensions.insert(ext);
	}
	stream.close();

	LoadPackagesInfo();
	loadFeatureExceptions();

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/features", distroString);
	stream.open(s);
	if (!stream) {
		BuildFeatures();
		snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/features", distroString);
		stream.open(s);
		if (!stream) {
			fprintf(stderr, "Can't open features\n");
			exit(1);
		}
	}
	while (!stream.eof()) {
		std::string str, fname, freqStr, normalFeature;
		int freq;
		size_t n1, n2;

		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		str = std::string(s);
		n1 = str.find_first_of('/');
		n2 = str.find_last_of('/');
		if (n1 == std::string::npos || n2 == std::string::npos)
			goto err;
		fname = str.substr(0, n1);
		normalizeFeature(normalFeature, fname);
		normalFeatures[fname] = normalFeature;
		freqStr = str.substr(n2 + 1);
		freq = strtol(freqStr.c_str(), NULL, 10);
		if (freq >= 1) {
			idf[normalFeature] += freq;
		}
	}
	stream.close();
	for (	idfIter  = idf.begin();
		idfIter != idf.end();
		idfIter++)
	{
		idfIter->second = log(numPackages/idfIter->second);
		if (idfIter->second > maxWeight) {
			maxWeight = idfIter->second;
		}
	}
	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/ignore-list-for-features", distroString);
	stream.open(s);
	if (!stream) {
		fprintf(stderr, "Couldn't open ignore-these-features\n");
		exit(1);
	}
	while (!stream.eof()) {
		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		featureSet.insert(s);
	}
	stream.close();
	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/ignore-these-false-positives", distroString);
	ReadIgnoreList(s);
	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/ignore-these-fixed", distroString);
	ReadIgnoreList(s);
	for (	pIter  = packages.begin();
		pIter != packages.end();
		pIter++)
	{
		filename = std::string("/var/lib/Clonewise/distros/") + distroString + std::string("/signatures/") + pIter->first;
		LoadSignature(filename, packagesSignatures[pIter->first]);
	}

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/model", distroString);
	if (access(s, R_OK) != 0) {
		if (getuid() != 0) {
			fprintf(stderr, "Need to be root to build database.\n");
			exit(1);
		}
		trainModel();
	}

	return 0;
err:
	printf("error\n");
	return 1;
}

int
RunClonewise(int argc, char *argv[])
{
	std::string filename;
	std::map<std::string, std::set<std::string> > embedding;
	std::map<std::string, std::list<std::string> >::const_iterator pIter;

	if (outputFormat == CLONEWISE_OUTPUT_XML) {
		printf("<Clonewise>\n");
	}
	if (doCheckRelated) {
		checkRelated();
	} if (allPackages == false) {
		for (int i = 0; i < argc; i++) {
			if (useRelativePathForSignature) {
				embedding = packagesSignatures[argv[i]];
			} else {
				filename = std::string(argv[i]);
				LoadSignature(filename, embedding);
			}
			checkPackage(embedding, argv[i]);
		}
	} else {
		for (	pIter  = packages.begin();
			pIter != packages.end();
			pIter++)
		{
			checkPackage(packagesSignatures[pIter->first], pIter->first.c_str());
		}
	}
	if (outputFormat == CLONEWISE_OUTPUT_XML) {
		printf("</Clonewise>\n");
	}
	return 0;
}

static void
CreateFeatures()
{
	std::ofstream rawFeaturesFile, featuresFile, fileCountFile;
	std::ifstream stream;
	char s[1024];
	std::set<std::string> featuresKey;
	std::map<std::string, int> features, featuresDoc;
	std::map<std::string, int>::iterator iter;
	std::multimap<int, std::string> iFeatures;
	std::multimap<int, std::string>::reverse_iterator iIter;
	std::map<std::string, std::list<std::string> >::const_iterator pIter;

	LoadPackagesInfo();

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/raw-features", distroString);
	rawFeaturesFile.open(s);
	if (!rawFeaturesFile) {
		fprintf(stderr, "Couldn't open raw-features\n");
		exit(1);
	}
	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/files-count", distroString);
	fileCountFile.open(s);
	if (!rawFeaturesFile) {
		fprintf(stderr, "Couldn't open files-count\n");
		exit(1);
	}
	for (	pIter  = packages.begin();
		pIter != packages.end();
		pIter++)
	{
		std::string filename;
		int i;

		filename = std::string("/var/lib/Clonewise/distros/") + distroString + std::string("/signatures/") + pIter->first;
		stream.open(filename.c_str());
		if (!stream) {
			errorLog("Couldn't open %s\n", filename.c_str());
		}
		i = 0;
		while (!stream.eof()) {
			std::string str, str2, hash, feature;
			char s[1024];
			size_t n, n1, n2;

			stream.getline(s, sizeof(s));
			if (s[0] == 0)
				break;
			lineToFeature(s, feature, hash);
			rawFeaturesFile << feature.c_str() << "\n";
			i++;
		}
		rawFeaturesFile << "/\n";
		fileCountFile << i << "\n";
		stream.close();
	}
	rawFeaturesFile.close();
	fileCountFile.close();

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/features", distroString);
	featuresFile.open(s);
	if (!featuresFile) {
		fprintf(stderr, "Couldn't open features\n");
		exit(1);
	}

	snprintf(s, sizeof(s), "/var/lib/Clonewise/distros/%s/features/raw-features", distroString);
	stream.open(s);
	if (!stream) {
		fprintf(stderr, "Couldn't open raw-features\n");
		exit(1);
	}
	while (!stream.eof()) {
		stream.getline(s, sizeof(s));
		if (s[0] == 0)
			break;
		if (s[0] == '/') {
			std::set<std::string>::iterator iter2;

			for (	iter2  = featuresKey.begin();
				iter2 != featuresKey.end();
				iter2++)
			{
				featuresDoc[*iter2]++;
			}
			featuresKey = std::set<std::string>();
		} else {
			featuresKey.insert(s);
printf("# feature %s %i\n", s, features[s]);
fflush(stdout);
			features[s]++;
		}
	}
	for (	iter  = features.begin();
		iter != features.end();
		iter++)
	{
		iFeatures.insert(std::pair<int, std::string>(iter->second, iter->first));
	}
	for (	iIter  = iFeatures.rbegin();
		iIter != iFeatures.rend();
		iIter++)
	{
		featuresFile << iIter->second.c_str() << "/" << iIter->first << "/" << featuresDoc[iIter->second] << "\n";
	}
printf("done\n");
fflush(stdout);
	featuresFile.close();
}
