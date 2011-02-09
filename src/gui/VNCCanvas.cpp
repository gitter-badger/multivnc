

#include <wx/sizer.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include "res/vnccursor.xbm"
#include "res/vnccursor-mask.xbm"
#include "MultiVNCApp.h"
#include "VNCCanvas.h"



/********************************************

  VNCCanvas class

********************************************/


BEGIN_EVENT_TABLE(VNCCanvas, wxPanel)
    EVT_PAINT  (VNCCanvas::onPaint)
    EVT_MOUSE_EVENTS (VNCCanvas::onMouseAction)
    EVT_KEY_DOWN (VNCCanvas::onKeyDown)
    EVT_KEY_UP (VNCCanvas::onKeyUp)
    EVT_CHAR (VNCCanvas::onChar)
    EVT_KILL_FOCUS(VNCCanvas::onFocusLoss)
    EVT_VNCCONNUPDATENOTIFY (wxID_ANY, VNCCanvas::onVNCConnUpdateNotify)
END_EVENT_TABLE();



/*
  constructor/destructor 
  (make sure size is set to 0,0 ow win32 gets stuck sending 
  paint events in listen mode)
*/

VNCCanvas::VNCCanvas(wxWindow* parent, VNCConn* c):
  wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(0,0), wxWANTS_CHARS)
{
  conn = c;
  adjustSize(); 
 
  // this kinda cursor creation works everywhere
  wxBitmap vnccursor_bitmap(vnccursor_bits, 16, 16);
  wxBitmap vnccursor_mask_bitmap(vnccursor_mask, 16, 16);
  vnccursor_bitmap.SetMask(new wxMask(vnccursor_mask_bitmap));
  wxImage vnccursor_image = vnccursor_bitmap.ConvertToImage();
  vnccursor_image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_X, 8);
  vnccursor_image.SetOption(wxIMAGE_OPTION_CUR_HOTSPOT_Y, 8);
  SetCursor(wxCursor(vnccursor_image));
}



/*
  private members
*/

void VNCCanvas::onPaint(wxPaintEvent &WXUNUSED(event))
{
  // this happens on GTK even if our size is (0,0)
  if(GetSize().GetWidth() == 0 || GetSize().GetHeight() == 0)
    return;

  wxPaintDC dc(this);

  // get the update rect list
  wxRegionIterator upd(GetUpdateRegion()); 
  while(upd)
    {
      wxRect update_rect(upd.GetRect());
     
      wxLogDebug(wxT("VNCCanvas %p: got repaint event: (%i,%i,%i,%i)"),
		 this,
		 update_rect.x,
		 update_rect.y,
		 update_rect.width,
		 update_rect.height);
      
    
      const wxBitmap& region = conn->getFrameBufferRegion(update_rect);
      dc.DrawBitmap(region, update_rect.x, update_rect.y);
	
      ++upd;
    }
}






void VNCCanvas::onMouseAction(wxMouseEvent &event)
{
  if(event.Entering())
    {
      SetFocus();
      
      wxCriticalSectionLocker lock(wxGetApp().mutex_theclipboard); 
      
      if(wxTheClipboard->Open()) 
	{
	  if(wxTheClipboard->IsSupported(wxDF_TEXT))
	    {
	      wxTextDataObject data;
	      wxTheClipboard->GetData(data);
	      wxString text = data.GetText();
	      wxLogDebug(wxT("VNCCanvas %p: setting cuttext: '%s'"), this, text.c_str());
	      conn->setCuttext(text);
	    }
	  wxTheClipboard->Close();
	}
    }

  conn->sendPointerEvent(event);
}


void VNCCanvas::onKeyDown(wxKeyEvent &event)
{
  conn->sendKeyEvent(event, true, false);
}


void VNCCanvas::onKeyUp(wxKeyEvent &event)
{
  conn->sendKeyEvent(event, false, false);
}


void VNCCanvas::onChar(wxKeyEvent &event)
{
  conn->sendKeyEvent(event, true, true);
}

void VNCCanvas::onFocusLoss(wxFocusEvent &event)
{
  wxLogDebug(wxT("VNCCanvas %p: lost focus, upping key modifiers"), this);
  
  wxKeyEvent key_event;

  key_event.m_keyCode = WXK_SHIFT;
  conn->sendKeyEvent(key_event, false, false);
  key_event.m_keyCode = WXK_ALT;
  conn->sendKeyEvent(key_event, false, false);
  key_event.m_keyCode = WXK_CONTROL;
  conn->sendKeyEvent(key_event, false, false);
}



void VNCCanvas::onVNCConnUpdateNotify(VNCConnUpdateNotifyEvent& event)
{
  VNCConn* sending_conn = static_cast<VNCConn*>(event.GetEventObject());

  // only do something if this is our VNCConn
  if(sending_conn == conn)
    drawRegion(event.rect);
}


/*
  public members
*/

void VNCCanvas::drawRegion(wxRect& rect)
{
#ifdef __WXDEBUG__
  wxLongLong t0 = wxGetLocalTimeMillis();
#endif

  wxClientDC dc(this);

  const wxBitmap& region = conn->getFrameBufferRegion(rect);
  dc.DrawBitmap(region, rect.x, rect.y);

#ifdef __WXDEBUG__
  wxLongLong t1 = wxGetLocalTimeMillis();
  wxLogDebug(wxT("VNCCanvas %p: drawing region (%i,%i,%i,%i) size %d took %lld ms"),
	     this,
	     rect.x,
	     rect.y,
	     rect.width,
	     rect.height,
	     rect.width * rect.height,
	     (t1-t0).GetValue());
#endif

}



void VNCCanvas::adjustSize()
{
  wxLogDebug(wxT("VNCCanvas %p: adjusting size to (%i, %i)"),
	     this,
	     conn->getFrameBufferWidth(),
	     conn->getFrameBufferHeight());

  // SetSize() isn't enough...
  SetInitialSize(wxSize(conn->getFrameBufferWidth(), conn->getFrameBufferHeight()));

  CentreOnParent();
  GetParent()->Layout();
}






/********************************************

  VNCCanvasContainer class

********************************************/

#define VNCCANVASCONTAINER_SCROLL_RATE 10
#define VNCCANVASCONTAINER_STATS_TIMER_INTERVAL 100
#define VNCCANVASCONTAINER_STATS_TIMER_ID 0

// map recv of custom events to handler methods
BEGIN_EVENT_TABLE(VNCCanvasContainer, wxScrolledWindow)
  EVT_TIMER   (VNCCANVASCONTAINER_STATS_TIMER_ID, VNCCanvasContainer::onStatsTimer)
END_EVENT_TABLE()


/*
  constructor/destructor
 */

VNCCanvasContainer::VNCCanvasContainer(wxWindow* parent):
  wxScrolledWindow(parent)
{
  canvas = 0;
  SetScrollRate(VNCCANVASCONTAINER_SCROLL_RATE, VNCCANVASCONTAINER_SCROLL_RATE);

  // a sizer dividing the container vertically
  SetSizer(new wxBoxSizer(wxVERTICAL));

  // insert a static box sizer into one slot
  sizer_stats_staticbox = new wxStaticBox(this, -1, _("Statistics"));
  wxStaticBoxSizer* sizer_stats = new wxStaticBoxSizer(sizer_stats_staticbox, wxHORIZONTAL);
  GetSizer()->Add(sizer_stats, 0, wxALIGN_CENTER_HORIZONTAL|wxALIGN_BOTTOM|wxALL, 3);

  // create statitistics widgets
  label_updrawbytes = new wxStaticText(this, wxID_ANY, _("Raw KB/s:"));
  text_ctrl_updrawbytes = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,18), wxTE_READONLY);
  label_updcount = new wxStaticText(this, wxID_ANY, _("Updates/s:"));
  text_ctrl_updcount = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,18), wxTE_READONLY);
  label_latency = new wxStaticText(this, wxID_ANY, _("Latency ms:"));
  text_ctrl_latency = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,18), wxTE_READONLY);
  label_lossratio = new wxStaticText(this, wxID_ANY, _("Loss Ratio:"));
  text_ctrl_lossratio = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80,18), wxTE_READONLY);
  label_recvbuf = new wxStaticText(this, wxID_ANY, _("Rcv Buffer:"));
  gauge_recvbuf = new wxGauge(this, wxID_ANY, 10, wxDefaultPosition, wxSize(80,18), wxGA_HORIZONTAL|wxGA_SMOOTH);

  dflt_fg = gauge_recvbuf->GetForegroundColour();

  // create grid sizers, two cause we wanna hide the multicast one sometimes
  wxGridSizer* grid_sizer_stats_uni = new wxGridSizer(2, 3, 0, 0);
  wxGridSizer* grid_sizer_stats_multi = new wxGridSizer(2, 2, 0, 0);
  // insert widgets into grid sizers
  grid_sizer_stats_uni->Add(label_updrawbytes, 0, wxALL, 3);
  grid_sizer_stats_uni->Add(label_updcount, 0, wxALL, 3);
  grid_sizer_stats_uni->Add(label_latency, 0, wxALL, 3);
  grid_sizer_stats_uni->Add(text_ctrl_updrawbytes, 0, wxLEFT|wxRIGHT|wxBOTTOM, 3);
  grid_sizer_stats_uni->Add(text_ctrl_updcount, 0, wxLEFT|wxRIGHT|wxBOTTOM, 3);
  grid_sizer_stats_uni->Add(text_ctrl_latency, 0, wxLEFT|wxRIGHT|wxBOTTOM, 3);
  grid_sizer_stats_multi->Add(label_lossratio, 0, wxALL, 3);
  grid_sizer_stats_multi->Add(label_recvbuf, 0, wxALL, 3);
  grid_sizer_stats_multi->Add(text_ctrl_lossratio, 0, wxLEFT|wxRIGHT|wxBOTTOM, 3);
  grid_sizer_stats_multi->Add(gauge_recvbuf, 0, wxLEFT|wxRIGHT|wxBOTTOM, 3);
  // insert grid sizer into static box sizer
  sizer_stats->Add(grid_sizer_stats_uni, 0, 0, 0); 
  sizer_stats->Add(grid_sizer_stats_multi, 0, 0, 0); 

  stats_timer.SetOwner(this, VNCCANVASCONTAINER_STATS_TIMER_ID);
}




VNCCanvasContainer::~VNCCanvasContainer()
{
  if(canvas)
    delete canvas;
}



/*
  private members
*/

void VNCCanvasContainer::onStatsTimer(wxTimerEvent& event)
{
  if(canvas && canvas->getConn())
    {
      const VNCConn* c = canvas->getConn();

      text_ctrl_updrawbytes->Clear();
      text_ctrl_updcount->Clear();
      text_ctrl_latency->Clear();
      text_ctrl_lossratio->Clear();

      if(!c->isMulticast())
	{
	  label_lossratio->Show(false);
	  text_ctrl_lossratio->Show(false);
	  label_recvbuf->Show(false);
	  gauge_recvbuf->Show(false);
	}
      else
	{
	  label_lossratio->Show(true);
	  text_ctrl_lossratio->Show(true);
	  label_recvbuf->Show(true);
	  gauge_recvbuf->Show(true);
	}
      Layout();
      
      if( ! c->getUpdRawByteStats().IsEmpty() )
	*text_ctrl_updrawbytes << wxAtoi(c->getUpdRawByteStats().Last().AfterLast(wxT(',')))/1024;
      if( ! c->getUpdCountStats().IsEmpty() )
	*text_ctrl_updcount << c->getUpdCountStats().Last().AfterLast(wxT(','));
      if( ! c->getLatencyStats().IsEmpty() )
	*text_ctrl_latency << c->getLatencyStats().Last().AfterLast(wxT(','));
      if( ! c->getMCLossRatioStats().IsEmpty() )
	*text_ctrl_lossratio << c->getMCLossRatioStats().Last().AfterLast(wxT(','));

      gauge_recvbuf->SetRange(c->getMCBufSize());
      gauge_recvbuf->SetValue(c->getMCBufFill());

      // flash red when buffer full
      if(gauge_recvbuf->GetRange() == gauge_recvbuf->GetValue())
	label_recvbuf->SetForegroundColour(*wxRED);
      else
	label_recvbuf->SetForegroundColour(dflt_fg);
    }
}



/*
  public members
*/

void VNCCanvasContainer::setCanvas(VNCCanvas* c)
{
  canvas = c;

  wxBoxSizer* sizer_vert = new wxBoxSizer(wxHORIZONTAL);
  sizer_vert->Add(c, 3, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL);
  GetSizer()->Insert(0, sizer_vert, 1, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL);
}



VNCCanvas* VNCCanvasContainer::getCanvas() const
{
  return canvas;
}


void VNCCanvasContainer::showStats(bool show_stats)
{
  if(show_stats)
    {
      stats_timer.Start(VNCCANVASCONTAINER_STATS_TIMER_INTERVAL);
      GetSizer()->Show(1, true);
    }
  else
    {
      stats_timer.Stop();
      GetSizer()->Show(1, false);
    }
  Layout();

  text_ctrl_updrawbytes->Clear();
  text_ctrl_updcount->Clear();
  text_ctrl_latency->Clear();
  text_ctrl_lossratio->Clear();
  gauge_recvbuf->SetValue(0);
}
