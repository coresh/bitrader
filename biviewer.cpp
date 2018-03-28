#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <gtk/gtk.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <wordexp.h>

using namespace std;

// Path to the binary data file containing historical trading data.
string historyPath = "$HOME/.bitrader/history";

struct Trade
{
	double price;
	double qty;
	long id, time;
	bool isBestMatch;
	bool isBuyerMaker;
};

struct OHLC
{
	double open;
	double high;
	double low;
	double close;
	
	OHLC() : open(HUGE_VAL), high(-HUGE_VAL), low(HUGE_VAL) { }
};

struct Viewport
{
	uint32_t width, height;
	
	Viewport(uint32_t width_, uint32_t height_) : width(width_), height(height_) { }
};

struct Symbol
{
	map<string, vector<OHLC> > candles;
	long startTime;
	size_t maxSecond;
};

map<string, Symbol> symbols;

class CandleDrawer
{
	const Viewport& viewport;

	void drawLine(cairo_t* cr, uint32_t position, uint32_t top, uint32_t bottom)
	{
		cairo_move_to(cr, position * CANDLE_WIDTH + CANDLE_WIDTH / 2, viewport.height - top);
		cairo_line_to(cr, position * CANDLE_WIDTH + CANDLE_WIDTH / 2, viewport.height - bottom);
		cairo_close_path(cr);
		cairo_stroke(cr);
	}

	void drawRectangle(cairo_t* cr, uint32_t position, uint32_t top, uint32_t bottom, bool filled)
	{
		cairo_rectangle(cr, position * CANDLE_WIDTH + 1, viewport.height - top, CANDLE_WIDTH - 2, top - bottom);
		if (filled) 
			cairo_fill(cr);
		else
			cairo_stroke(cr);
	}

public :

	static const uint32_t CANDLE_WIDTH = 10;
	
	void draw(cairo_t* cr, uint32_t position, uint32_t open, uint32_t high, uint32_t low, uint32_t close)
	{
		drawRectangle(cr, position, open, close, close > open);

		if (open > close)
		{
			drawLine(cr, position, close, low);
			drawLine(cr, position, high, open);
		}
		else
		{
			drawLine(cr, position, open, low);
			drawLine(cr, position, high, close);
		}
	}
	
	CandleDrawer(const Viewport& viewport_) : viewport(viewport_) { }
};

class ChartDrawer
{
	const Viewport& viewport;

public :

	void draw(cairo_t* cr, size_t& position, const vector<OHLC>& candles, size_t szcandles)
	{
		GdkRGBA color;
		color.red = 21 / 256.0;
		color.green = 26 / 256.0;
		color.blue = 29 / 256.0;
		color.alpha = 1.0;
		gdk_cairo_set_source_rgba(cr, &color);

		cairo_rectangle(cr, 0, 0, viewport.width, viewport.height);
		cairo_fill(cr);

		cairo_set_line_width (cr, 1);
		color.red = 49 / 256.0;
		color.green = 58 / 256.0;
		color.blue = 66 / 256.0;
		color.alpha = 1.0;
		gdk_cairo_set_source_rgba(cr, &color);

		const uint32_t ngridlines = 10;
		uint32_t step = viewport.height / ngridlines;
		for (uint32_t i = 0; i < ngridlines; i++)
		{
			cairo_move_to(cr, 0, i * step);
			cairo_line_to(cr, viewport.width, i * step);
			cairo_close_path(cr);
			cairo_stroke(cr);
		}

		color.red = 240 / 256.0;
		color.green = 184 / 256.0;
		color.blue = 12 / 256.0;
		color.alpha = 1.0;
		gdk_cairo_set_source_rgba(cr, &color);

		uint32_t ncandles = viewport.width / CandleDrawer::CANDLE_WIDTH;
		if (viewport.width % CandleDrawer::CANDLE_WIDTH) ncandles++;

		CandleDrawer candleDrawer(viewport);
		
		// Do not allow position to shrink the last right-most visible candles window.
		position = min(position, szcandles - ncandles);

		double minval = HUGE_VAL, maxval = -HUGE_VAL;
		for (uint32_t e = szcandles, i = e - ncandles; i < e; i++)
		{
			const OHLC& candle = candles[i - position];

			if (!isfinite(candle.low)) continue;
			if (!isfinite(candle.high)) continue;
		
			minval = fmin(minval, candle.low);
			maxval = fmax(maxval, candle.high);
		}
		
		double scale = (maxval - minval) / viewport.height;

		for (size_t e = szcandles, i = e - ncandles, ii = 0; i < e; i++, ii++)
		{
			const OHLC& candle = candles[i - position];

			if (!isfinite(candle.low)) continue;
			if (!isfinite(candle.high)) continue;
			
			candleDrawer.draw(cr, ii,
				(candle.open - minval) / scale, (candle.high - minval) / scale,
				(candle.low - minval) / scale, (candle.close - minval) / scale);
		}		

		cairo_set_line_width(cr, 2);
		color.red = 49 / 256.0;
		color.green = 58 / 256.0;
		color.blue = 66 / 256.0;
		color.alpha = 1.0;
		gdk_cairo_set_source_rgba(cr, &color);
		cairo_rectangle(cr, 0, 0, viewport.width, viewport.height);
		cairo_stroke(cr);
	}

	ChartDrawer(const Viewport& viewport_) : viewport(viewport_) { }
};

class AnnotatedChartObject;

class ChartObject
{
	bool isScrolling;
	gdouble start;
	size_t position;

	static gboolean onDraw(GtkWidget* widget, cairo_t* cr, gpointer data)
	{
		ChartObject* chart = (ChartObject*)data;

		guint width = gtk_widget_get_allocated_width(widget);
		guint height = gtk_widget_get_allocated_height(widget);
		Viewport viewport(width, height);

		ChartDrawer chartDrawer(viewport);
		chartDrawer.draw(cr, chart->position, symbols["BATBTC"].candles["1min"], symbols["BATBTC"].maxSecond + 1);

		return FALSE;
	}

	static gboolean onMouse(GtkWidget *widget, GdkEventButton* event, gpointer data)
	{
		if (event->button == 1)
		{
			ChartObject* chart = (ChartObject*)data;

			if (event->type == GDK_BUTTON_PRESS)		
			{
				chart->isScrolling = true;
				chart->start = event->x;
			}
			else if (event->type = GDK_BUTTON_RELEASE)
			{
				chart->isScrolling = false;
			}
		}
		
		return TRUE;
	}

	static gboolean onMove(GtkWidget *widget, GdkEventMotion* event, gpointer data)
	{
		ChartObject* chart = (ChartObject*)data;
		
		if (chart->isScrolling)
		{
			if (chart->position >= chart->start - event->x)
				chart->position -= chart->start - event->x;
			else
				chart->position = 0;
				
			chart->start = event->x;		
			gtk_widget_queue_draw(widget);
		}
	
		return TRUE;
	}
	
public :

	ChartObject(GtkWidget* window) : isScrolling(false), position(0)
	{
		GtkWidget* drawing_area = gtk_drawing_area_new();
		gtk_container_add(GTK_CONTAINER(window), drawing_area);
		gtk_widget_set_size_request(drawing_area, 800, 600);

		gtk_widget_set_events(GTK_WIDGET(drawing_area), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
		g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(ChartObject::onDraw), this);
		g_signal_connect(G_OBJECT(drawing_area), "button_press_event", G_CALLBACK(ChartObject::onMouse), this);
		g_signal_connect(G_OBJECT(drawing_area), "button_release_event", G_CALLBACK(ChartObject::onMouse), this);
		g_signal_connect(G_OBJECT(drawing_area), "motion_notify_event", G_CALLBACK(ChartObject::onMove), this);
	}
	
	friend class AnnotatedChartObject;
};

class AnnotatedChartObject
{
	ChartObject chart;
	GtkWidget* grid;

	static gboolean onDraw(GtkWidget* widget, cairo_t* cr, gpointer data)
	{
		AnnotatedChartObject* aco = (AnnotatedChartObject*)data;

		guint width = gtk_widget_get_allocated_width(widget);
		guint height = gtk_widget_get_allocated_height(widget);
		Viewport viewport(width, height);

		ChartDrawer chartDrawer(viewport);
		chartDrawer.draw(cr, aco->chart.position, symbols["BATBTC"].candles["1min"], symbols["BATBTC"].maxSecond + 1);

		return FALSE;
	}

public :

	AnnotatedChartObject(GtkWidget* window) : chart(window)
	{
		grid = gtk_grid_new();
		gtk_container_add(GTK_CONTAINER(window), grid);
		gtk_widget_set_size_request(grid, 800, 600);

		g_signal_connect(G_OBJECT(grid), "draw", G_CALLBACK(AnnotatedChartObject::onDraw), this);

//		gtk_grid_attach(grid, chart.widget, 0, 0, 1, 1);
	}
};

class Archive
{
	archive* a;
	bool isOpen;

public :

	bool is_open() const { return isOpen; }

	bool readNextHeader()
	{
		struct archive_entry *entry;
		int r = archive_read_next_header(a, &entry);
		cout << "Found entry " << archive_entry_pathname(entry) << endl;
		return (r == ARCHIVE_OK);
	}

	ssize_t readData(char* buffer, size_t szbuffer)
	{
		return archive_read_data(a, buffer, szbuffer);
	}

	Archive(const string& filename) : a(archive_read_new()), isOpen(false)
	{
		archive_read_support_format_tar(a);
		archive_read_support_filter_bzip2(a); // TODO
		int r = archive_read_open_filename(a, filename.c_str(), 16384);
		isOpen = (r == ARCHIVE_OK);
	}
	
	~Archive()
	{
		archive_read_free(a);
	}
};

gint main(int argc, char *argv[])
{
	// Expand the history path.
	{
		wordexp_t p;
		char** w;
		wordexp(historyPath.c_str(), &p, 0);
		w = p.we_wordv;
		historyPath = w[0];
		wordfree(&p);
	}
	
	// Find all files in the history path.
	vector<string> historyFiles;
	while (1)
	{
		dirent* dirEntry = NULL;
		DIR* dir = opendir(historyPath.c_str());
		if (!dir) break;
		bool targetLibraryFound = false;
		while ((dirEntry = readdir(dir)) != NULL)
		{
			bool hasExpectedExtension = true;
			string name(dirEntry->d_name);
			const string extension = ".tar.bz2";
			if (name.size() < extension.size())
				hasExpectedExtension = false;
			else
			{
				for (int i = 0; i < extension.size(); i++)
					if (name[name.size() - i - 1] != extension[extension.size() - i - 1])
					{
						hasExpectedExtension = false;
						break;
					}
			}
			if (hasExpectedExtension)
			{
				const string historyFilename = historyPath + "/" + dirEntry->d_name;
				historyFiles.push_back(historyFilename);
			}
		}
		closedir(dir);
		break;
	}

	// For each history file
	for (int i = 0, e = historyFiles.size(); i < e; i++)
	{
		const string& historyFile = historyFiles[i];
		
		// Get symbol name.
		string name = "";
		for (int j = historyFile.size() - 1; j >= 0; j--)
		{
			if (historyFile[j] != '/') continue;
			
			for (int k = j + 1; k < historyFile.size(); k++)
			{
				if (historyFile[k] != '.') continue;
				
				name = string(historyFile.c_str() + j + 1, k - j - 1);
				break;
			}
			break;
		}
		
		if (name == "")
		{
			fprintf(stderr, "Cannot determine symbol name for file %s\n", historyFile.c_str());
			continue;
		}
		
		cout << "Loading historical data for symbol " << name << " ... " << endl;
		
		Symbol& symbol = symbols[name];

		cout << "Decompressing " << historyFile << endl;

		Archive archive(historyFile);
		if (!archive.is_open())
		{
			fprintf(stderr, "Error opening compressed file %s\n", historyFile.c_str());
			continue;
		}
		if (!archive.readNextHeader())
		{
			fprintf(stderr, "Error reading archive header from compressed file %s\n", historyFile.c_str());
			continue;
		}
		
		symbol.maxSecond = 0;
		bool firstTrade = true;
		const size_t szbatch = 1024;
		for (;;)
		{
			vector<Trade> trades(szbatch);
			ssize_t size = archive.readData((char*)&trades[0], szbatch * sizeof(Trade));
			if (size < 0)
			{
				fprintf(stderr, "Error reading data from compressed file %s\n", historyFile.c_str());
				break;
			}
			if (size == 0) break;
			
			for (int k = 0, ke = min(szbatch, size / sizeof(Trade)); k < ke; k++)
			{
				const Trade& trade = trades[k];
				
				if (firstTrade)
				{
					symbol.startTime = trade.time;
					firstTrade = false;
				}
				
				const uint64_t second = (trade.time - symbol.startTime) / (1000 * 60 * 30);
				symbol.maxSecond = max(symbol.maxSecond, second);
				vector<OHLC>& candles = symbol.candles["1min"];
				if (candles.size() <= second)
					candles.resize(candles.size() + szbatch);
				
				OHLC& candle = candles[second];
				candle.high = fmax(candle.high, trade.price);
				candle.low = fmin(candle.low, trade.price);
				if (candle.open == HUGE_VAL)
					candle.open = trade.price;
				candle.close = trade.price;
			}
		}
	}

	gtk_init(&argc, &argv);
	
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_icon_name(GTK_WINDOW(window), "binance");
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	AnnotatedChartObject chart(window);

	gtk_widget_show_all(window);
	gtk_main();

	return 0;
}

