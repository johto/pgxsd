#!/usr/bin/perl

use strict;
use warnings;

use DBI;
use DBD::Pg;
use File::Spec;
use XML::LibXML;
use Test::More;

sub print_usage {
    my $fh = shift;
    print $fh "Usage:\n";
    print $fh "  $0: { OPTION | TESTDIR }\n";
    print $fh "\n";
    print $fh "Options:\n";
    print $fh "  -h, --help   Display this help, then exit\n";
    print $fh "\n";
    print $fh "The database to connect to, the user to connect as, etc. can be\n";
    print $fh "configured through the PG* environment variables supported by libpq.\n";
    print $fh "Please refer to the libpq documentation for more information.\n";
}

if ((scalar @ARGV) != 1) {
    print_usage(*STDERR);
    exit(1);
} elsif ($ARGV[0] eq '-h' || $ARGV[0] eq '--help') {
    print_usage(*STDOUT);
    exit(0);
}
my $testdir = $ARGV[0];

my @connect = ("dbi:Pg:", '', '', {pg_enable_utf8 => 1, RaiseError => 1, PrintError => 0, AutoCommit => 1});
my $dbh = DBI->connect(@connect) or die "Unable to connect to PostgreSQL";

# Make sure we have the latest schema for the extension
$dbh->do("DROP EXTENSION IF EXISTS pgxsd");
$dbh->do("CREATE EXTENSION pgxsd");

# For some reason DBD::Pg doesn't allow you to extract the DETAIL from the
# error message.  Create a simple wrapper function to extract it instead.
$dbh->do(q{
CREATE FUNCTION pg_temp.run_pgxsd_test(OUT o_sqlstate text, OUT o_detail text, _doc xml, _schema text) RETURNS RECORD
AS $$
BEGIN
    PERFORM pgxsd.schema_validate(_doc, _schema);
    o_sqlstate := '00000';
    o_detail := NULL;
    RETURN;
EXCEPTION WHEN invalid_xml_document THEN
    GET STACKED DIAGNOSTICS
        o_sqlstate  = RETURNED_SQLSTATE,
        o_detail    = PG_EXCEPTION_DETAIL;
    RETURN;
END
$$ LANGUAGE plpgsql;
});

opendir(my $dirfh, $testdir) or
    die "could not open directory $testdir: $!\n";
while (my $fname = readdir($dirfh)) {
    unless ($fname =~ qr/^([0-9]{3}(?:-[a-zA-Z_]+))\.xml$/) {
        next;
    }
    my $testname = $1;

    my $fpath = File::Spec->catfile($testdir, $fname);
    my $xmldoc;

    $xmldoc = XML::LibXML->load_xml(location => $fpath);
    execute_test($testname, $xmldoc);
}
closedir($dirfh) or
    die "could not close directory $testdir: $!\n";

done_testing();

sub execute_test {
    my ($testname, $xmldoc) = @_;

    my $document_from_node = sub {
        my $node = shift;

        my $doc = XML::LibXML::Document->new('1.0', 'UTF-8');
        $doc->setDocumentElement($node);
        my $xml = $doc->toString(0);
        if (!utf8::decode($xml)) {
            die "could not UTF-8 decode document\n";
        }
        return $xml;
    };

    my $testRootSchemaLocation = undef;

    my $xc = XML::LibXML::XPathContext->new($xmldoc);
    my @xsdnodes = $xc->findnodes('//xsd');
    if ((scalar @xsdnodes) < 1) {
        die "could not find any XSD definitions";
    }
    foreach my $xsd (@xsdnodes) {
        my $schemaLocation = $xsd->getAttribute('schemaLocation');
        if (!defined($testRootSchemaLocation)) {
            $testRootSchemaLocation = $schemaLocation;
        }

        my $xsdxc = XML::LibXML::XPathContext->new($xsd);
        $xsdxc->registerNs('xs', 'http://www.w3.org/2001/XMLSchema');
        my @xsnodes = $xsdxc->findnodes('//xsd/xs:schema');
        if ((my $nxsnodes = (scalar @xsnodes)) != 1) {
            die "unexpected number of xs:schema nodes $nxsnodes\n";
        }
        my $xsdxml = $document_from_node->($xsnodes[0]);
        $dbh->do(q{
            INSERT INTO pgxsd.schemata
                (schema_location, document)
            VALUES ($1, $2)
        }, undef, $schemaLocation, $xsdxml);
    }

    my @xmlnodes = $xc->findnodes('//xml');
    if ((my $nxmlnodes = (scalar @xmlnodes)) != 1) {
        die "unexpected number of xml nodes $nxmlnodes\n";
    }
    my $input_xml = $document_from_node->($xmlnodes[0]);

    my $expect_success = $xc->exists('//result/success');

    my $rows = $dbh->selectall_arrayref(q{
        SELECT o_sqlstate, o_detail FROM pg_temp.run_pgxsd_test($1, $2)
    }, undef, $input_xml, $testRootSchemaLocation);
    if ((my $num_rows = (scalar @$rows)) != 1) {
        die "unexpected number of rows $num_rows";
    }
    my $data = $rows->[0];
    if ((my $num_cols = (scalar @$data)) != 2) {
        die "unexpected number of columns $num_cols";
    }
    my ($sqlstate, $details) = @$data;

    if ($sqlstate eq '00000') {
        if ($expect_success) {
            return ok(1, $testname);
        } else {
            return ok(0, $testname);
        }
    } elsif ($sqlstate eq '2200M') {
        if ($expect_success) {
            diag "Expected success, but validation failed with the following details:  \n";
            foreach my $detail (split(/[\r\n]+/, $details)) {
                diag "  $detail\n";
            }
            return ok(0, $testname);
        } else {
            return ok(1, $testname);
        }
        print "$details\n";
    } else {
        die "unexpected SQLSTATE $sqlstate";
    }
}
