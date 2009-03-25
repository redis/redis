package Redis::Hash;

use strict;
use warnings;

use Tie::Hash;
use base qw/Redis Tie::StdHash/;

use Data::Dump qw/dump/;

=head1 NAME

Redis::Hash - tie perl hashes into Redis

=head1 SYNOPSYS

  tie %name, 'Redis::Hash', 'prefix';

=cut

# mandatory methods
sub TIEHASH {
	my ($class,$name) = @_;
	my $self = Redis->new;
	$name .= ':' if $name;
	$self->{name} = $name || '';
	bless $self => $class;
}

sub STORE {
	my ($self,$key,$value) = @_;
	$self->set( $self->{name} . $key, $value );
}

sub FETCH {
	my ($self,$key) = @_;
	$self->get( $self->{name} . $key );
}

sub FIRSTKEY {
	my $self = shift;
	$self->{keys} = [ $self->keys( $self->{name} . '*' ) ];
	$self->NEXTKEY;
} 

sub NEXTKEY {
	my $self = shift;
	my $key = shift @{ $self->{keys} } || return;
	my $name = $self->{name};
	$key =~ s{^$name}{} || warn "can't strip $name from $key";
	return $key;
}

sub EXISTS {
	my ($self,$key) = @_;
	$self->exists( $self->{name} . $key );
}

sub DELETE {
	my ($self,$key) = @_;
	$self->del( $self->{name} . $key );
}

sub CLEAR {
	my ($self) = @_;
	$self->del( $_ ) foreach ( $self->keys( $self->{name} . '*' ) );
	$self->{keys} = [];
}

1;
