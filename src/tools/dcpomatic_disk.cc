/*
    Copyright (C) 2019-2020 Carl Hetherington <cth@carlh.net>

    This file is part of DCP-o-matic.

    DCP-o-matic is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DCP-o-matic is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DCP-o-matic.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "wx/wx_signal_manager.h"
#include "wx/wx_util.h"
#include "wx/job_manager_view.h"
#include "wx/drive_wipe_warning_dialog.h"
#include "wx/try_unmount_dialog.h"
#include "wx/message_dialog.h"
#include "lib/file_log.h"
#include "lib/dcpomatic_log.h"
#include "lib/util.h"
#include "lib/config.h"
#include "lib/signal_manager.h"
#include "lib/cross.h"
#include "lib/copy_to_drive_job.h"
#include "lib/job_manager.h"
#include "lib/disk_writer_messages.h"
#include <wx/wx.h>
#include <boost/process.hpp>
#ifdef DCPOMATIC_WINDOWS
#include <boost/process/windows.hpp>
#endif
#ifdef DCPOMATIC_OSX
#include <ApplicationServices/ApplicationServices.h>
#endif

using std::string;
using std::exception;
using std::cout;
using std::cerr;
using boost::shared_ptr;
using boost::optional;

class DOMFrame : public wxFrame
{
public:
	explicit DOMFrame (wxString const & title)
		: wxFrame (0, -1, title)
		, _nanomsg (true)
		, _sizer (new wxBoxSizer(wxVERTICAL))
	{
		/* Use a panel as the only child of the Frame so that we avoid
		   the dark-grey background on Windows.
		*/
		wxPanel* overall_panel = new wxPanel (this);
		wxSizer* s = new wxBoxSizer (wxHORIZONTAL);
		s->Add (overall_panel, 1, wxEXPAND);
		SetSizer (s);

		wxGridBagSizer* grid = new wxGridBagSizer (DCPOMATIC_SIZER_X_GAP, DCPOMATIC_SIZER_Y_GAP);

		int r = 0;
		add_label_to_sizer (grid, overall_panel, _("DCP"), true, wxGBPosition(r, 0));
		wxBoxSizer* dcp_name_sizer = new wxBoxSizer (wxHORIZONTAL);
		_dcp_name = new wxStaticText (overall_panel, wxID_ANY, wxEmptyString);
		dcp_name_sizer->Add (_dcp_name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, DCPOMATIC_SIZER_X_GAP);
		_dcp_open = new wxButton (overall_panel, wxID_ANY, _("Open..."));
		dcp_name_sizer->Add (_dcp_open, 0);
		grid->Add (dcp_name_sizer, wxGBPosition(r, 1), wxDefaultSpan, wxEXPAND);
		++r;

		add_label_to_sizer (grid, overall_panel, _("Drive"), true, wxGBPosition(r, 0));
		wxBoxSizer* drive_sizer = new wxBoxSizer (wxHORIZONTAL);
		_drive = new wxChoice (overall_panel, wxID_ANY);
		drive_sizer->Add (_drive, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, DCPOMATIC_SIZER_X_GAP);
		_drive_refresh = new wxButton (overall_panel, wxID_ANY, _("Refresh"));
		drive_sizer->Add (_drive_refresh, 0);
		grid->Add (drive_sizer, wxGBPosition(r, 1), wxDefaultSpan, wxEXPAND);
		++r;

		_jobs = new JobManagerView (overall_panel, false);
		grid->Add (_jobs, wxGBPosition(r, 0), wxGBSpan(6, 2), wxEXPAND);
		r += 6;

		_copy = new wxButton (overall_panel, wxID_ANY, _("Copy DCP"));
		grid->Add (_copy, wxGBPosition(r, 0), wxGBSpan(1, 2), wxEXPAND);
		++r;

		grid->AddGrowableCol (1);

		_dcp_open->Bind (wxEVT_BUTTON, boost::bind(&DOMFrame::open, this));
		_copy->Bind (wxEVT_BUTTON, boost::bind(&DOMFrame::copy, this));
		_drive->Bind (wxEVT_CHOICE, boost::bind(&DOMFrame::setup_sensitivity, this));
		_drive_refresh->Bind (wxEVT_BUTTON, boost::bind(&DOMFrame::drive_refresh, this));

		_sizer->Add (grid, 1, wxALL | wxEXPAND, DCPOMATIC_DIALOG_BORDER);
		overall_panel->SetSizer (_sizer);
		Fit ();
		SetSize (768, GetSize().GetHeight() + 32);

		/* XXX: this is a hack, but I expect we'll need logs and I'm not sure if there's
		 * a better place to put them.
		 */
		dcpomatic_log.reset(new FileLog(config_path() / "disk.log"));
		dcpomatic_log->set_types (dcpomatic_log->types() | LogEntry::TYPE_DISK);
		LOG_DISK_NC("dcpomatic_disk started");

		drive_refresh ();

		Bind (wxEVT_SIZE, boost::bind (&DOMFrame::sized, this, _1));

		JobManager::instance()->ActiveJobsChanged.connect(boost::bind(&DOMFrame::setup_sensitivity, this));

#ifdef DCPOMATIC_WINDOWS
		/* We must use ::shell here, it seems, to avoid error code 740 (related to privilege escalation) */
		LOG_DISK("Starting writer process %1", disk_writer_path().string());
		_writer = new boost::process::child (disk_writer_path(), boost::process::shell, boost::process::windows::hide);
#endif

#ifdef DCPOMATIC_LINUX
		LOG_DISK("Starting writer process %1", disk_writer_path().string());
		_writer = new boost::process::child (disk_writer_path());
#endif

		/* _writer is always running on macOS at the moment */
	}

	~DOMFrame ()
	{
		_nanomsg.send(DISK_WRITER_QUIT "\n", 2000);
	}

private:
	void sized (wxSizeEvent& ev)
	{
		_sizer->Layout ();
		ev.Skip ();
	}

	void open ()
	{
		wxDirDialog* d = new wxDirDialog (this, _("Choose a DCP folder"), wxT(""), wxDD_DIR_MUST_EXIST);
		int r = d->ShowModal ();
		boost::filesystem::path const path (wx_to_std(d->GetPath()));
		d->Destroy ();

		if (r != wxID_OK) {
			return;
		}

		_dcp_path = path;
		_dcp_name->SetLabel (std_to_wx(_dcp_path->filename().string()));
		setup_sensitivity ();
	}

	void copy ()
	{
		DCPOMATIC_ASSERT (_drive->GetSelection() != wxNOT_FOUND);
		DCPOMATIC_ASSERT (static_cast<bool>(_dcp_path));

		Drive const& drive = _drives[_drive->GetSelection()];
		if (drive.mounted()) {
			TryUnmountDialog* d = new TryUnmountDialog(this, drive.description());
			int const r = d->ShowModal ();
			d->Destroy ();
			if (r != wxID_OK) {
				return;
			}

			if (!_nanomsg.send(DISK_WRITER_UNMOUNT "\n", 2000)) {
				throw CommunicationFailedError ();
			}
			if (!_nanomsg.send(drive.internal_name() + "\n", 2000)) {
				throw CommunicationFailedError ();
			}
			optional<string> reply = _nanomsg.receive (2000);
			if (!reply || *reply != DISK_WRITER_OK) {
				MessageDialog* m = new MessageDialog (
						this,
						_("DCP-o-matic Disk Writer"),
						wxString::Format(_("The drive %s could not be unmounted.\nClose any application that is using it, then try again."), std_to_wx(drive.description()))
						);
				m->ShowModal ();
				m->Destroy ();
				return;
			}
		}

		
		DriveWipeWarningDialog* d = new DriveWipeWarningDialog (this, _drive->GetString(_drive->GetSelection()));
		int const r = d->ShowModal ();
		bool ok = r == wxID_OK && d->confirmed();
		d->Destroy ();

		if (!ok) {
			return;
		}

		JobManager::instance()->add(shared_ptr<Job>(new CopyToDriveJob(*_dcp_path, _drives[_drive->GetSelection()], _nanomsg)));
		setup_sensitivity ();
	}

	void drive_refresh ()
	{
		int const sel = _drive->GetSelection ();
		wxString current;
		if (sel != wxNOT_FOUND) {
			current = _drive->GetString (sel);
		}
		_drive->Clear ();
		int re_select = wxNOT_FOUND;
		int j = 0;
		_drives = get_drives ();
		BOOST_FOREACH (Drive i, _drives) {
			wxString const s = std_to_wx(i.description());
			if (s == current) {
				re_select = j;
			}
			_drive->Append(s);
			++j;
		}
		_drive->SetSelection (re_select);
		setup_sensitivity ();
	}

	void setup_sensitivity ()
	{
		_copy->Enable (static_cast<bool>(_dcp_path) && _drive->GetSelection() != wxNOT_FOUND && !JobManager::instance()->work_to_do());
	}

	wxStaticText* _dcp_name;
	wxButton* _dcp_open;
	wxChoice* _drive;
	wxButton* _drive_refresh;
	wxButton* _copy;
	JobManagerView* _jobs;
	boost::optional<boost::filesystem::path> _dcp_path;
	std::vector<Drive> _drives;
	boost::process::child* _writer;
	Nanomsg _nanomsg;
	wxSizer* _sizer;
};

class App : public wxApp
{
public:
	App ()
		: _frame (0)
	{}

	bool OnInit ()
	{
		try {
			Config::FailedToLoad.connect (boost::bind (&App::config_failed_to_load, this));
			Config::Warning.connect (boost::bind (&App::config_warning, this, _1));

			SetAppName (_("DCP-o-matic Disk Writer"));

			if (!wxApp::OnInit()) {
				return false;
			}

#ifdef DCPOMATIC_LINUX
			unsetenv ("UBUNTU_MENUPROXY");
#endif

#ifdef __WXOSX__
			ProcessSerialNumber serial;
			GetCurrentProcess (&serial);
			TransformProcessType (&serial, kProcessTransformToForegroundApplication);
#endif

			dcpomatic_setup_path_encoding ();

			/* Enable i18n; this will create a Config object
			   to look for a force-configured language.  This Config
			   object will be wrong, however, because dcpomatic_setup
			   hasn't yet been called and there aren't any filters etc.
			   set up yet.
			*/
			dcpomatic_setup_i18n ();

			/* Set things up, including filters etc.
			   which will now be internationalised correctly.
			*/
			dcpomatic_setup ();

			/* Force the configuration to be re-loaded correctly next
			   time it is needed.
			*/
			Config::drop ();

			_frame = new DOMFrame (_("DCP-o-matic Disk Writer"));
			SetTopWindow (_frame);

			_frame->Show ();

			signal_manager = new wxSignalManager (this);
			Bind (wxEVT_IDLE, boost::bind (&App::idle, this, _1));
		}
		catch (exception& e)
		{
			error_dialog (0, wxString::Format ("DCP-o-matic could not start."), std_to_wx(e.what()));
		}

		return true;
	}

	void config_failed_to_load ()
	{
		message_dialog (_frame, _("The existing configuration failed to load.  Default values will be used instead.  These may take a short time to create."));
	}

	void config_warning (string m)
	{
		message_dialog (_frame, std_to_wx(m));
	}

	void idle (wxIdleEvent& ev)
	{
		signal_manager->ui_idle ();
		ev.Skip ();
	}

	DOMFrame* _frame;
};

IMPLEMENT_APP (App)
