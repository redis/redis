package Redis::List;

use strict;
use warnings;

use base qw/Redis Tie::Array/;

=head1 NAME

Redis::List - tie perl arrays into Redis lists

=head1 SYNOPSYS

  tie @a, 'Redis::List', 'name';

=cut

# mandatory methods
sub TIEARRAY {
	my ($class,$name) = @_;
	my $self = $class->new;
	$self->{name} = $name;
	bless $self => $class;
}

sub FETCH {
	my ($self,$index) = @_;
	$self->lindex( $self->{name}, $index );
}

sub FETCHSIZE {
	my ($self) = @_;
	$self->llen( $self->{name} );
} 

sub STORE {
	my ($self,$index,$value) = @_;
	$self->lset( $self->{name}, $index, $value );
}

sub STORESIZE {
	my ($self,$count) = @_;
	$self->ltrim( $self->{name}, 0, $count );
#		if $count > $self->FETCHSIZE;
}

sub CLEAR {
	my ($self) = @_;
	$self->del( $self->{name} );
}

sub PUSH {
	my $self = shift;
	$self->rpush( $self->{name}, $_ ) foreach @_;
}

sub SHIFT {
	my $self = shift;
	$self->lpop( $self->{name} );
}

sub UNSHIFT {
	my $self = shift;
	$self->lpush( $self->{name}, $_ ) foreach @_;
}

sub SPLICE {
	my $self = shift;
	my $offset = shift;
	my $length = shift;
	$self->lrange( $self->{name}, $offset, $length );
	# FIXME rest of @_ ?
}

sub EXTEND {
	my ($self,$count) = @_;
	$self->rpush( $self->{name}, '' ) foreach ( $self->FETCHSIZE .. ( $count - 1 ) );
} 

sub DESTROY {
	my $self = shift;
	$self->quit;
}

1;
