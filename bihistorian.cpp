#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <jsoncpp/json/json.h>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <wordexp.h>

#include "binance.h"

using namespace binance;
using namespace std;

struct Trade
{
	char symbol[8];
	double price;
	double qty;
	long id, time;
	bool isBestMatch;
	bool isBuyerMaker;
};

// Path to the binary data file containing historical trading data.
string history_path = "$HOME/.bitrader/history.dat";

string msSinceEpochToDate(long milliseconds)
{
	long seconds = milliseconds / 1000;

	string date = asctime(localtime(&seconds));
	string::size_type i = date.find("\n");
	if (i != string::npos)
		date.erase(i, date.length());
	
	stringstream result;
	result << date;
	result << " + ";
	result << milliseconds % 1000;
	result << " ms";
	
	return result.str();
}

int main()
{
	cout << "Initializing ..." << endl;

	{
		wordexp_t p;
		char** w;
		wordexp(history_path.c_str(), &p, 0);
		w = p.we_wordv;
		history_path = w[0];
		wordfree(&p);
	}
	
	Server server;

	Market market(server);

	Account account(server);
	if (!account.keysAreSet())
	{
		fprintf(stderr, "\nCannot find the api/secret keys pair for Binance account!\n");
		fprintf(stderr, "The user should either provide them to Account constructor,\n");
		fprintf(stderr, "or in the following files: %s, %s\n\n",
			binance::Account::default_api_key_path.c_str(),
			binance::Account::default_secret_key_path.c_str());

		exit(1);
	}

	cout << "Getting all trading pairs ..." << endl;

	vector<string> pairs;
	map<string, int> pairsmap;

	// Get all pairs.
	{
		Json::Value symbols;
		BINANCE_ERR_CHECK(market.getAllPrices(symbols));
		
		pairs.resize(symbols.size());
		for (Json::Value::ArrayIndex i = 0; i < pairs.size(); i++)
		{
			const string& symbol = symbols[i]["symbol"].asString();
			
			pairs[i] = symbol;
			
			// Trim symbol to 8 chars max (incl. '\0'), as we have in our database.
			Trade trade;
			pairsmap[string(symbol.c_str(), symbol.c_str() + min(symbol.size(), sizeof(trade.symbol) - 1))] = i + 1;
		}
	}
	
	vector<long> minIds(pairs.size());
	vector<long> minTimes(pairs.size());
	for (int i = 0; i < pairs.size(); i++)
		minIds[i] = numeric_limits<long>::max();

	ifstream history(history_path.c_str(), ifstream::binary);
	if (history.is_open())
	{
		history.seekg(0, history.end);
		size_t length = history.tellg();
	    
		if (length % sizeof(Trade))
		{
			fprintf(stderr, "File length %zu is not a multiply of the trade record size %zu\n", 
				length, sizeof(Trade));
			fprintf(stderr, "malformed history data file or invalid format?\n");
			exit(-1);
		}
	
		cout << "Reading existing historical data file ..." << endl;

		history.seekg(0, history.beg);

		const size_t szbatch = 1024;
		for (size_t j = 0, je = length / sizeof(Trade); j < je; j += szbatch)
		{
			vector<Trade> trades(szbatch);
			history.read((char*)&trades[0], sizeof(Trade) * min(szbatch, je - j));
			if (history.rdstate())
			{
				fprintf(stderr, "Error reading historical data file: ");
				if ((history.rdstate() & ifstream::eofbit) != 0)
					fprintf(stderr, "End-of-File reached on input operation\n");
				else if ((history.rdstate() & ifstream::failbit) != 0)
					fprintf(stderr, "Logical error on i/o operation\n");
				else if ((history.rdstate() & ifstream::badbit) != 0)
					fprintf(stderr, "Read/writing error on i/o operation\n");
				exit(1);
			}
			
			for (int k = 0, ke = min(szbatch, je - j); k < ke; k++)
			{
				const Trade& trade = trades[k];
				
				int i = pairsmap[trade.symbol] - 1;
				if (i == -1)
				{
					fprintf(stderr, "Cannot find symbol \"%s\" in pairsmap\n", trade.symbol);
					exit(1);
				}

				long& minTime = minTimes[i];
				long& minId = minIds[i];

				if (minId > trade.id)
				{
					minId = trade.id;
					minTime = trade.time;
				}
			}
		}

		history.close();

		for (int i = 0; i < pairs.size(); i++)
		{
			const string& symbol = pairs[i];

			const long& minTime = minTimes[i];
			const long& minId = minIds[i];

			if (minId == numeric_limits<long>::max())
				cout << symbol << " : no data" << endl;
			else
				cout << symbol << " : " << minId << " (" << msSinceEpochToDate(minTime) << ")" << endl;
		}
		
		cout << "OK" << endl;
	}

	cout << "Retrieving historical trades ..." << endl;

	// Get historical trades for all *BTC pairs.
	#pragma omp parallel for num_threads(6) schedule(dynamic, 1)
	for (int i = 0; i < pairs.size(); i++)
	{
		const string& symbol = pairs[i];

		long& minId = minIds[i];
		while (minId > 0)
		{
			Json::Value result;
			while (1)
			{
				binanceError_t status = binanceSuccess;
				
				if (minId != numeric_limits<long>::max())
					status = account.getHistoricalTrades(result, symbol.c_str(), max(0L, minId - 500 - 1));
				else
					status = account.getHistoricalTrades(result, symbol.c_str());

				if (status == binanceErrorEmptyServerResponse) continue;
			
				BINANCE_ERR_CHECK(status);
			
				break;
			}

			long minTime;
			vector<Trade> trades(result.size());
			for (Json::Value::ArrayIndex j = 0; j < result.size(); j++)
			{
				Trade& trade = trades[j];

				memcpy(&trade.symbol[0], symbol.c_str(), min(sizeof(trade.symbol), symbol.size() + 1));
				trade.symbol[7] = '\0';
				trade.id = atol(result[j]["id"].asString().c_str());
				trade.isBestMatch = result[j]["isBestMatch"].asString() == "true";
				trade.isBuyerMaker = result[j]["isBuyerMaker"].asString() == "true";
				trade.price = atof(result[j]["price"].asString().c_str());
				trade.qty = atof(result[j]["qty"].asString().c_str());			
				trade.time = atol(result[j]["time"].asString().c_str());
			
				if (minId > trade.id)
				{
					minId = trade.id;
					minTime = trade.time;
				}
			}

			#pragma omp critical
			{
				ofstream history(history_path.c_str(), ofstream::binary | ifstream::app);
				if (!history.is_open())
				{
					fprintf(stderr, "Cannot open history file for writing: %s\n", history_path.c_str());
					exit(1);
				}
				history.write((char*)&trades[0], sizeof(Trade) * trades.size());
				history.close();
			}
		
			cout << symbol << " : " << minId << " (" << msSinceEpochToDate(minTime) << ")" << endl;
		}
	}

	return 0;
}

