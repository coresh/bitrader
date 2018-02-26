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

	Market market;

	Account account;
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

	Json::Value pairs;

	// Get all pairs.
	BINANCE_ERR_CHECK(market.getAllPrices(pairs)); 
	
	vector<long> minIds(pairs.size());
	for (int i = 0; i < pairs.size(); i++)
		minIds[i] = numeric_limits<long>::max();

	ifstream history(history_path.c_str(), ifstream::binary | ifstream::app);
	if (history.is_open())
	{
		history.seekg(0, history.end);
	    int length = history.tellg();
	    
	    if (length % sizeof(Trade))
	    {
	    	fprintf(stderr, "File length %d is not a multiply of the trade record size %zu\n",
	    		length, sizeof(Trade));
	    	fprintf(stderr, "malformed history data file or invalid format?\n");
	    	exit(-1);
	    }
	
		cout << "Reading existing historical data file ..." << endl;

		for (Json::Value::ArrayIndex i = 0; i < pairs.size(); i++)
		{
			const string& symbol = pairs[i]["symbol"].asString();
			
			history.seekg(0, history.beg);

			long minTime;
			long& minId = minIds[i];
			for (int j = 0, e = length / sizeof(Trade); j < e; j++)
			{
				Trade trade;
				history.read((char*)&trade, sizeof(trade));
				
				if ((string)trade.symbol != symbol) continue;

				if (minId > trade.id)
				{
					minId = trade.id;
					minTime = trade.time;
				}
			}

			if (minId == numeric_limits<long>::max())
				cout << symbol << " : no data" << endl;
			else
				cout << symbol << " : " << minId << " (" << msSinceEpochToDate(minTime) << ")" << endl;
		}
		
		history.close();
		
		cout << "OK" << endl;
	}

	cout << "Retrieving historical trades ..." << endl;

	// Get historical trades for all *BTC pairs.
	#pragma omp parallel for num_threads(4)
	for (Json::Value::ArrayIndex i = 0; i < pairs.size(); i++)
	{
		const string& symbol = pairs[i]["symbol"].asString();

		long& minId = minIds[i];
		while (minId > 0)
		{
			Json::Value result;
			if (minId != numeric_limits<long>::max())
				BINANCE_ERR_CHECK(account.getHistoricalTrades(result, symbol.c_str(), max(0L, minId - 500 - 1)));
			else
				BINANCE_ERR_CHECK(account.getHistoricalTrades(result, symbol.c_str()));

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

