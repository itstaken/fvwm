# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

package FVWM::Module::Gtk;

use 5.004;
use strict;
use vars qw($VERSION @ISA @EXPORT);
use Exporter;

use FVWM::Module;

BEGIN {
	# try to load Gtk, otherwise show a nice error box if possible
	eval "use Gtk;";
	if ($@) {
		my $scriptName = $0; $scriptName =~ s|.*/||;
		my $errorTitle = 'FVWM Perl library error';
		my $errorMsg = "$scriptName requires Gtk-Perl package to be installed";
		print STDERR "[$errorTitle]: $errorMsg\n$@";
		if (-x '/usr/bin/gdialog' || -x '/usr/bin/X11/gdialog') {
			system("gdialog --title '$errorTitle' --msgbox '$errorMsg' 500 100");
		} elsif (-x '/usr/bin/gtk-shell' || -x '/usr/bin/X11/gtk-shell') {
			system("gtk-shell --size 500 100 --title '$errorTitle' --label '$errorMsg' --button Close");
		} elsif (-x '/usr/bin/xmessage' || -x '/usr/bin/X11/xmessage') {
			system("xmessage -name '$errorTitle' '$errorMsg'");
		}
		exit(1);
	}
};

@ISA = qw(FVWM::Module Exporter);
@EXPORT = @FVWM::Module::EXPORT;
$VERSION = $FVWM::Module::VERSION;

sub eventLoop ($) {
	my $self = shift;

	Gtk::Gdk->input_add(
		$self->{istream}->fileno, ['read'],
		sub ($$$) {
			#my ($socket, $fd, $flags) = @_;
			#return 0 unless $flags->{'read'};
			unless ($self->processPacket($self->readPacket)) {
				Gtk->main_quit;
			}
			return 1;
		}
	);
	Gtk->main;
	$self->debug("exited gtk event loop", 2);
	$self->disconnect;
}

sub openErrorDialog ($$;$) {
	my $self = shift;
	my $error = shift;
	my $title = shift || ($self->name . " Error");

	my $dialog = new Gtk::Dialog;
	$dialog->set_title($title);
	$dialog->set_border_width(4);

	my $label = new Gtk::Label $error;
	$dialog->vbox->pack_start($label, 0, 1, 10);

	my $button = new Gtk::Button "Close";
	$dialog->action_area->pack_start($button, 1, 1, 0);
	$button->signal_connect("clicked", sub { $dialog->destroy; });

	$button = new Gtk::Button "Close All Errors";
	$dialog->action_area->pack_start($button, 1, 1, 0);
	$button->signal_connect("clicked",
		sub { $self->send("All ('$title') Close"); });

	$button = new Gtk::Button "Exit Module";
	$dialog->action_area->pack_start($button, 1, 1, 0);
	$button->signal_connect("clicked", sub { Gtk->main_quit; });

	$dialog->show_all;
}

sub openMessageDialog ($$;$) {
	my $self = shift;
	my $message = shift;
	my $title = shift || ($self->name . " Message");

	my $dialog = new Gtk::Dialog;
	$dialog->set_title($title);
	$dialog->set_border_width(4);

	my $label = new Gtk::Label $message;
	$dialog->vbox->pack_start($label, 0, 1, 10);

	my $button = new Gtk::Button "Close";
	$dialog->action_area->pack_start($button, 1, 1, 0);
	$button->signal_connect("clicked", sub { $dialog->destroy; });

	$dialog->show_all;
}

sub addDefaultErrorHandler ($) {
	my $self = shift;

	$self->addHandler(M_ERROR, sub {
		my ($self, $event) = @_;
		$self->openErrorDialog($event->_text, "FVWM Error");
	});
}

1;

__END__

=head1 NAME

FVWM::Module::Gtk - FVWM::Module with the GTK+ widget library attached

=head1 SYNOPSIS

  use Gtk;
  use lib `fvwm-perllib dir`;
  use FVWM::Module::Gtk;

  my $module = new FVWM::Module::Gtk;

  init Gtk;
  my $window = new Gtk::Window -toplevel;;
  my $label = new Gtk::Label "Hello, world";
  $window->add($label);
  $window->show_all;

  $module->addHandler(M_CONFIGURE_WINDOW, \&configure_toplevel);
  $module->addHandler(M_CONFIG_INFO, \&some_other_sub);

  $module->eventLoop;

=head1 DESCRIPTION

The B<FVWM::Module::Gtk> package is a sub-class of B<FVWM::Module> that
overloads the methods B<eventLoop> and B<addDefaultErrorHandler> to manage
GTK+ objects as well. It also adds new methods B<openErrorDialog> and
B<openMessageDialog>.

This manual page details only those differences. For details on the
API itself, see L<FVWM::Module>.

=head1 METHODS

Only those methods that are not available in B<FVWM::Module> or overloaded ones
are covered here:

=over 8

=item B<eventLoop>

From outward appearances, this methods operates just as the parent
B<eventLoop> does. It is worth mentioning, however, that this version
enters into the B<Gtk>->B<main> subroutine, ostensibly not to return.

=item B<openErrorDialog> I<error> [I<title>]

This method creates a dialog box using the GTK+ widgets. The dialog has
three buttons labeled "Close", "Close All Errors" and "Exit Module".
Selecting the "Close" button closes the dialog. "Close All Errors" closes
all error dialogs that may be open on the screen at that time.
"Exit Module" terminates your entire module.

Good for debugging a GTK+ based module.

=item B<openMessageDialog> I<message> [I<title>]

Creates a message window with one "Close" button.

=item B<addDefaultErrorHandler>

This methods adds a M_ERROR handler to automatically notify you that an error
has been reported by FVWM. The M_ERROR handler then calls C<openErrorDialog()>
with the received error text as a parameter to show it in a window.

=head1 BUGS

Awaiting for your reporting.

=head1 CAVEATS

In keeping with the UNIX philosophy, B<FVWM::Module> does not keep you from
doing stupid things, as that would also keep you from doing clever things.
What this means is that there are several areas with which you can hang your
module or even royally confuse your running I<fvwm> process. This is due to
flexibility, not bugs.

=head1 AUTHOR

Mikhael Goikhman <migo@homemail.com>.

=head1 THANKS TO

Randy J. Ray <randy@byz.org>.

Kenneth Albanowski, Paolo Molaro for Gtk/Perl extension.

=head1 SEE ALSO

For more information, see L<fvwm>, L<FVWM::Module> and L<Gtk>.

=cut