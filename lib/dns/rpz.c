/*
 * Copyright (C) 2011-2013  Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id$ */


/*! \file */

#include <config.h>

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/net.h>
#include <isc/netaddr.h>
#include <isc/print.h>
#include <isc/stdlib.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/fixedname.h>
#include <dns/log.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/result.h>
#include <dns/rbt.h>
#include <dns/rpz.h>
#include <dns/view.h>


/*
 * Parallel radix trees for databases of response policy IP addresses
 *
 * The radix or patricia trees are somewhat specialized to handle response
 * policy addresses by representing the two sets of IP addresses and name
 * server IP addresses in a single tree.  One set of IP addresses is
 * for rpz-ip policies or policies triggered by addresses in A or
 * AAAA records in responses.
 * The second set is for rpz-nsip policies or policies triggered by addresses
 * in A or AAAA records for NS records that are authorities for responses.
 *
 * Each leaf indicates that an IP address is listed in the IP address or the
 * name server IP address policy sub-zone (or both) of the corresponding
 * response response zone.  The policy data such as a CNAME or an A record
 * is kept in the policy zone.  After an IP address has been found in a radix
 * tree, the node in the policy zone's database is found by converting
 * the IP address to a domain name in a canonical form.
 *
 *
 * The response policy zone canonical form of an IPv6 address is one of:
 *	prefix.W.W.W.W.W.W.W.W
 *	prefix.WORDS.zz
 *	prefix.WORDS.zz.WORDS
 *	prefix.zz.WORDS
 *  where
 *	prefix	is the prefix length of the IPv6 address between 1 and 128
 *	W	is a number between 0 and 65535
 *	WORDS	is one or more numbers W separated with "."
 *	zz	corresponds to :: in the standard IPv6 text representation
 *
 * The canonical form of IPv4 addresses is:
 *	prefix.B.B.B.B
 *  where
 *	prefix	is the prefix length of the address between 1 and 32
 *	B	is a number between 0 and 255
 *
 * Names for IPv4 addresses are distinguished from IPv6 addresses by having
 * 5 labels all of which are numbers, and a prefix between 1 and 32.
 */


/*
 * Use a private definition of IPv6 addresses because s6_addr32 is not
 * always defined and our IPv6 addresses are in non-standard byte order
 */
typedef isc_uint32_t		dns_rpz_cidr_word_t;
#define DNS_RPZ_CIDR_WORD_BITS	((int)sizeof(dns_rpz_cidr_word_t)*8)
#define DNS_RPZ_CIDR_KEY_BITS	((int)sizeof(dns_rpz_cidr_key_t)*8)
#define DNS_RPZ_CIDR_WORDS	(128/DNS_RPZ_CIDR_WORD_BITS)
typedef struct {
	dns_rpz_cidr_word_t	w[DNS_RPZ_CIDR_WORDS];
} dns_rpz_cidr_key_t;

#define ADDR_V4MAPPED		0xffff
#define KEY_IS_IPV4(prefix,ip) ((prefix) >= 96 && (ip)->w[0] == 0 &&	\
				(ip)->w[1] == 0 && (ip)->w[2] == ADDR_V4MAPPED)

#define DNS_RPZ_WORD_MASK(b) ((b) == 0 ? (dns_rpz_cidr_word_t)(-1)	\
			      : ((dns_rpz_cidr_word_t)(-1)		\
				 << (DNS_RPZ_CIDR_WORD_BITS - (b))))

/*
 * Get bit #n from the array of words of an IP address.
 */
#define DNS_RPZ_IP_BIT(ip, n) (1 & ((ip)->w[(n)/DNS_RPZ_CIDR_WORD_BITS] >>  \
				    (DNS_RPZ_CIDR_WORD_BITS		    \
				     - 1 - ((n) % DNS_RPZ_CIDR_WORD_BITS))))

/*
 * A pair of arrays of bits flagging the existence of
 * IP or QNAME (d) and NSIP or NSDNAME (ns) policy triggers.
 */
typedef struct dns_rpz_pair_zbits dns_rpz_pair_zbits_t;
struct dns_rpz_pair_zbits {
	dns_rpz_zbits_t	    d;
	dns_rpz_zbits_t	    ns;
};

/*
 * A CIDR or radix tree node.
 */
struct dns_rpz_cidr_node {
	dns_rpz_cidr_node_t	*parent;
	dns_rpz_cidr_node_t	*child[2];
	dns_rpz_cidr_key_t	ip;
	dns_rpz_prefix_t	prefix;
	dns_rpz_pair_zbits_t	pair;
	dns_rpz_pair_zbits_t	sum;
};

/*
 * The data in a RBT node has two pairs of bits for policy zones.
 * One pair is for the corresponding name of the node such as example.com
 * and the other pair is for a wildcard child such as *.example.com.
 */
typedef struct dns_rpz_nm_data dns_rpz_nm_data_t;
struct dns_rpz_nm_data {
	dns_rpz_pair_zbits_t	pair;
	dns_rpz_pair_zbits_t	wild;
};

#if 0
/*
 * Catch a name while debugging.
 */
static void
catch_name(const dns_name_t *src_name, const char *tgt, const char *str) {
	dns_fixedname_t tgt_namef;
	dns_name_t *tgt_name;

	dns_fixedname_init(&tgt_namef);
	tgt_name = dns_fixedname_name(&tgt_namef);
	dns_name_fromstring(tgt_name, tgt, DNS_NAME_DOWNCASE, NULL);
	if (dns_name_equal(src_name, tgt_name)) {
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "rpz hit failed: %s %s", str, tgt);
	}
}
#endif

const char *
dns_rpz_type2str(dns_rpz_type_t type) {
	switch (type) {
	case DNS_RPZ_TYPE_QNAME:
		return ("QNAME");
	case DNS_RPZ_TYPE_IP:
		return ("IP");
	case DNS_RPZ_TYPE_NSIP:
		return ("NSIP");
	case DNS_RPZ_TYPE_NSDNAME:
		return ("NSDNAME");
	case DNS_RPZ_TYPE_BAD:
		break;
	}
	FATAL_ERROR(__FILE__, __LINE__, "impossible rpz type %d", type);
	return ("impossible");
}

dns_rpz_policy_t
dns_rpz_str2policy(const char *str) {
	if (str == NULL)
		return (DNS_RPZ_POLICY_ERROR);
	if (!strcasecmp(str, "given"))
		return (DNS_RPZ_POLICY_GIVEN);
	if (!strcasecmp(str, "disabled"))
		return (DNS_RPZ_POLICY_DISABLED);
	if (!strcasecmp(str, "passthru"))
		return (DNS_RPZ_POLICY_PASSTHRU);
	if (!strcasecmp(str, "nxdomain"))
		return (DNS_RPZ_POLICY_NXDOMAIN);
	if (!strcasecmp(str, "nodata"))
		return (DNS_RPZ_POLICY_NODATA);
	if (!strcasecmp(str, "cname"))
		return (DNS_RPZ_POLICY_CNAME);
	/*
	 * Obsolete
	 */
	if (!strcasecmp(str, "no-op"))
		return (DNS_RPZ_POLICY_PASSTHRU);
	return (DNS_RPZ_POLICY_ERROR);
}

const char *
dns_rpz_policy2str(dns_rpz_policy_t policy) {
	const char *str;

	switch (policy) {
	case DNS_RPZ_POLICY_PASSTHRU:
		str = "PASSTHRU";
		break;
	case DNS_RPZ_POLICY_NXDOMAIN:
		str = "NXDOMAIN";
		break;
	case DNS_RPZ_POLICY_NODATA:
		str = "NODATA";
		break;
	case DNS_RPZ_POLICY_RECORD:
		str = "Local-Data";
		break;
	case DNS_RPZ_POLICY_CNAME:
	case DNS_RPZ_POLICY_WILDCNAME:
		str = "CNAME";
		break;
	default:
		str = "";
		POST(str);
		INSIST(0);
	}
	return (str);
}

static int
zbit_to_num(dns_rpz_zbits_t zbit) {
	dns_rpz_num_t rpz_num;

	INSIST(zbit != 0);
	rpz_num = 0;
#if DNS_RPZ_MAX_ZONES > 32
	if ((zbit & 0xffffffff00000000L) != 0) {
		zbit >>= 32;
		rpz_num += 32;
	}
#endif
	if ((zbit & 0xffff0000) != 0) {
		zbit >>= 16;
		rpz_num += 16;
	}
	if ((zbit & 0xff00) != 0) {
		zbit >>= 8;
		rpz_num += 8;
	}
	if ((zbit & 0xf0) != 0) {
		zbit >>= 4;
		rpz_num += 4;
	}
	if ((zbit & 0xc) != 0) {
		zbit >>= 2;
		rpz_num += 2;
	}
	if ((zbit & 2) != 0)
		++rpz_num;
	return (rpz_num);
}

static inline void
make_pair(dns_rpz_pair_zbits_t *pair, dns_rpz_zbits_t zbits,
	  dns_rpz_type_t type)
{
	switch (type) {
	case DNS_RPZ_TYPE_QNAME:
	case DNS_RPZ_TYPE_IP:
		pair->d = zbits;
		pair->ns = 0;
		break;
	case DNS_RPZ_TYPE_NSDNAME:
	case DNS_RPZ_TYPE_NSIP:
		pair->d = 0;
		pair->ns = zbits;
		break;
	default:
		INSIST(0);
		break;
	}
}

/*
 * Mark a node and all of its parents as having IP or NSIP data
 */
static void
set_sum_pair(dns_rpz_cidr_node_t *cnode) {
	dns_rpz_cidr_node_t *child;
	dns_rpz_pair_zbits_t sum;

	do {
		sum = cnode->pair;

		child = cnode->child[0];
		if (child != NULL) {
			sum.d |= child->sum.d;
			sum.ns |= child->sum.ns;
		}

		child = cnode->child[1];
		if (child != NULL) {
			sum.d |= child->sum.d;
			sum.ns |= child->sum.ns;
		}

		if (cnode->sum.d == sum.d &&
		    cnode->sum.ns == sum.ns)
			break;
		cnode->sum = sum;
		cnode = cnode->parent;
	} while (cnode != NULL);
}

static void
fix_qname_skip_recurse(dns_rpz_zones_t *rpzs) {
	dns_rpz_zbits_t zbits;

	/*
	 * Get a mask covering all policy zones that are not subordinate to
	 * other policy zones containing triggers that require that the
	 * qname be resolved before they can be checked.
	 */
	if (rpzs->p.qname_wait_recurse) {
		zbits = 0;
	} else {
		zbits = (rpzs->have.ipv4 || rpzs->have.ipv6 ||
			 rpzs->have.nsdname ||
			 rpzs->have.nsipv4 || rpzs->have.nsipv6);
		if (zbits == 0) {
			zbits = DNS_RPZ_ALL_ZBITS;
		} else {
			zbits = DNS_RPZ_ZMASK(zbit_to_num(zbits));
		}
	}
	rpzs->have.qname_skip_recurse = zbits;

	rpzs->have.ip = rpzs->have.ipv4 | rpzs->have.ipv6;
	rpzs->have.nsip = rpzs->have.nsipv4 | rpzs->have.nsipv6;
}

static void
adj_trigger_cnt(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
		dns_rpz_type_t rpz_type,
		const dns_rpz_cidr_key_t *tgt_ip, dns_rpz_prefix_t tgt_prefix,
		isc_boolean_t inc)
{
	dns_rpz_zone_t *rpz;
	int *cnt;
	dns_rpz_zbits_t *have;

	rpz = rpzs->zones[rpz_num];
	switch (rpz_type) {
	case DNS_RPZ_TYPE_QNAME:
		cnt = &rpz->triggers.qname;
		have = &rpzs->have.qname;
		break;
	case DNS_RPZ_TYPE_IP:
		REQUIRE(tgt_ip != NULL);
		if (KEY_IS_IPV4(tgt_prefix, tgt_ip)) {
			cnt = &rpz->triggers.ipv4;
			have = &rpzs->have.ipv4;
		} else {
			cnt = &rpz->triggers.ipv6;
			have = &rpzs->have.ipv6;
		}
		break;
	case DNS_RPZ_TYPE_NSDNAME:
		cnt = &rpz->triggers.nsdname;
		have = &rpzs->have.nsdname;
		break;
	case DNS_RPZ_TYPE_NSIP:
		REQUIRE(tgt_ip != NULL);
		if (KEY_IS_IPV4(tgt_prefix, tgt_ip)) {
			cnt = &rpz->triggers.nsipv4;
			have = &rpzs->have.nsipv4;
		} else {
			cnt = &rpz->triggers.nsipv6;
			have = &rpzs->have.nsipv6;
		}
		break;
	default:
		INSIST(0);
	}

	if (inc) {
		if (++*cnt == 1) {
			*have |= DNS_RPZ_ZBIT(rpz_num);
			fix_qname_skip_recurse(rpzs);
		}
	} else {
		REQUIRE(*cnt > 0);
		if (--*cnt == 0) {
			*have &= ~DNS_RPZ_ZBIT(rpz_num);
			fix_qname_skip_recurse(rpzs);
		}
	}
}

static dns_rpz_cidr_node_t *
new_node(dns_rpz_zones_t *rpzs,
	 const dns_rpz_cidr_key_t *ip, dns_rpz_prefix_t prefix,
	 const dns_rpz_cidr_node_t *child)
{
	dns_rpz_cidr_node_t *new;
	int i, words, wlen;

	new = isc_mem_get(rpzs->mctx, sizeof(*new));
	if (new == NULL)
		return (NULL);
	memset(new, 0, sizeof(*new));

	if (child != NULL)
		new->sum = child->sum;

	new->prefix = prefix;
	words = prefix / DNS_RPZ_CIDR_WORD_BITS;
	wlen = prefix % DNS_RPZ_CIDR_WORD_BITS;
	i = 0;
	while (i < words) {
		new->ip.w[i] = ip->w[i];
		++i;
	}
	if (wlen != 0) {
		new->ip.w[i] = ip->w[i] & DNS_RPZ_WORD_MASK(wlen);
		++i;
	}
	while (i < DNS_RPZ_CIDR_WORDS)
		new->ip.w[i++] = 0;

	return (new);
}

static void
badname(int level, dns_name_t *name, const char *str1, const char *str2) {
	char namebuf[DNS_NAME_FORMATSIZE];

	/*
	 * bin/tests/system/rpz/tests.sh looks for "invalid rpz".
	 */
	if (level < DNS_RPZ_DEBUG_QUIET
	    && isc_log_wouldlog(dns_lctx, level)) {
		dns_name_format(name, namebuf, sizeof(namebuf));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, level,
			      "invalid rpz IP address \"%s\"%s%s",
			      namebuf, str1, str2);
	}
}

/*
 * Convert an IP address from radix tree binary (host byte order) to
 * to its canonical response policy domain name without the origin of the
 * policy zone.
 */
static isc_result_t
ip2name(const dns_rpz_cidr_key_t *tgt_ip, dns_rpz_prefix_t tgt_prefix,
	dns_name_t *base_name, dns_name_t *ip_name)
{
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif
	int w[DNS_RPZ_CIDR_WORDS*2];
	char str[1+8+1+INET6_ADDRSTRLEN+1];
	isc_buffer_t buffer;
	isc_result_t result;
	isc_boolean_t zeros;
	int i, n, len;

	if (KEY_IS_IPV4(tgt_prefix, tgt_ip)) {
		len = snprintf(str, sizeof(str), "%d.%d.%d.%d.%d",
			       tgt_prefix - 96,
			       tgt_ip->w[3] & 0xff,
			       (tgt_ip->w[3]>>8) & 0xff,
			       (tgt_ip->w[3]>>16) & 0xff,
			       (tgt_ip->w[3]>>24) & 0xff);
		if (len < 0 || len > (int)sizeof(str))
			return (ISC_R_FAILURE);
	} else {
		for (i = 0; i < DNS_RPZ_CIDR_WORDS; i++) {
			w[i*2+1] = ((tgt_ip->w[DNS_RPZ_CIDR_WORDS-1-i] >> 16)
				    & 0xffff);
			w[i*2] = tgt_ip->w[DNS_RPZ_CIDR_WORDS-1-i] & 0xffff;
		}
		zeros = ISC_FALSE;
		len = snprintf(str, sizeof(str), "%d", tgt_prefix);
		if (len == -1)
			return (ISC_R_FAILURE);
		i = 0;
		while (i < DNS_RPZ_CIDR_WORDS * 2) {
			if (w[i] != 0 || zeros
			    || i >= DNS_RPZ_CIDR_WORDS * 2 - 1
			    || w[i+1] != 0) {
				INSIST((size_t)len <= sizeof(str));
				n = snprintf(&str[len], sizeof(str) - len,
					     ".%x", w[i++]);
				if (n < 0)
					return (ISC_R_FAILURE);
				len += n;
			} else {
				zeros = ISC_TRUE;
				INSIST((size_t)len <= sizeof(str));
				n = snprintf(&str[len], sizeof(str) - len,
					     ".zz");
				if (n < 0)
					return (ISC_R_FAILURE);
				len += n;
				i += 2;
				while (i < DNS_RPZ_CIDR_WORDS * 2 && w[i] == 0)
					++i;
			}
			if (len >= (int)sizeof(str))
				return (ISC_R_FAILURE);
		}
	}

	isc__buffer_init(&buffer, str, sizeof(str));
	isc__buffer_add(&buffer, len);
	result = dns_name_fromtext(ip_name, &buffer, base_name, 0, NULL);
	return (result);
}

/*
 * Determine the type a of a name in a response policy zone.
 */
static dns_rpz_type_t
type_from_name(dns_rpz_zone_t *rpz, dns_name_t *name) {

	if (dns_name_issubdomain(name, &rpz->ip))
		return (DNS_RPZ_TYPE_IP);

	/*
	 * Require `./configure --enable-rpz-nsip` and nsdname
	 * until consistency problems are resolved.
	 */
#ifdef ENABLE_RPZ_NSIP
	if (dns_name_issubdomain(name, &rpz->nsip))
		return (DNS_RPZ_TYPE_NSIP);
#endif

#ifdef ENABLE_RPZ_NSDNAME
	if (dns_name_issubdomain(name, &rpz->nsdname))
		return (DNS_RPZ_TYPE_NSDNAME);
#endif

	return (DNS_RPZ_TYPE_QNAME);
}

/*
 * Convert an IP address from canonical response policy domain name form
 * to radix tree binary (host byte order) for adding or deleting IP or NSIP
 * data.
 */
static isc_result_t
name2ipkey(int log_level,
	   const dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	   dns_rpz_type_t rpz_type, dns_name_t *src_name,
	   dns_rpz_cidr_key_t *tgt_ip, dns_rpz_prefix_t *tgt_prefix,
	   dns_rpz_pair_zbits_t *new_pair)
{
	dns_rpz_zone_t *rpz;
	char ip_str[DNS_NAME_FORMATSIZE];
	dns_offsets_t ip_name_offsets;
	dns_fixedname_t ip_name2f;
	dns_name_t ip_name, *ip_name2;
	const char *prefix_str, *cp, *end;
	char *cp2;
	int ip_labels;
	dns_rpz_prefix_t prefix;
	unsigned long prefix_num, l;
	isc_result_t result;
	int i;

	REQUIRE(rpzs != NULL && rpz_num < rpzs->p.num_zones);
	rpz = rpzs->zones[rpz_num];
	REQUIRE(rpz != NULL);

	make_pair(new_pair, DNS_RPZ_ZBIT(rpz_num), rpz_type);

	ip_labels = dns_name_countlabels(src_name);
	if (rpz_type == DNS_RPZ_TYPE_QNAME)
		ip_labels -= dns_name_countlabels(&rpz->origin);
	else
		ip_labels -= dns_name_countlabels(&rpz->nsdname);
	if (ip_labels < 2) {
		badname(log_level, src_name, "; too short", "");
		return (ISC_R_FAILURE);
	}
	dns_name_init(&ip_name, ip_name_offsets);
	dns_name_getlabelsequence(src_name, 0, ip_labels, &ip_name);

	/*
	 * Get text for the IP address
	 */
	dns_name_format(&ip_name, ip_str, sizeof(ip_str));
	end = &ip_str[strlen(ip_str)+1];
	prefix_str = ip_str;

	prefix_num = strtoul(prefix_str, &cp2, 10);
	if (*cp2 != '.') {
		badname(log_level, src_name,
			"; invalid leading prefix length", "");
		return (ISC_R_FAILURE);
	}
	*cp2 = '\0';
	if (prefix_num < 1U || prefix_num > 128U) {
		badname(log_level, src_name,
			"; invalid prefix length of ", prefix_str);
		return (ISC_R_FAILURE);
	}
	cp = cp2+1;

	if (--ip_labels == 4 && !strchr(cp, 'z')) {
		/*
		 * Convert an IPv4 address
		 * from the form "prefix.w.z.y.x"
		 */
		if (prefix_num > 32U) {
			badname(log_level, src_name,
				"; invalid IPv4 prefix length of ", prefix_str);
			return (ISC_R_FAILURE);
		}
		prefix_num += 96;
		*tgt_prefix = (dns_rpz_prefix_t)prefix_num;
		tgt_ip->w[0] = 0;
		tgt_ip->w[1] = 0;
		tgt_ip->w[2] = ADDR_V4MAPPED;
		tgt_ip->w[3] = 0;
		for (i = 0; i < 32; i += 8) {
			l = strtoul(cp, &cp2, 10);
			if (l > 255U || (*cp2 != '.' && *cp2 != '\0')) {
				if (*cp2 == '.')
					*cp2 = '\0';
				badname(log_level, src_name,
					"; invalid IPv4 octet ", cp);
				return (ISC_R_FAILURE);
			}
			tgt_ip->w[3] |= l << i;
			cp = cp2 + 1;
		}
	} else {
		/*
		 * Convert a text IPv6 address.
		 */
		*tgt_prefix = (dns_rpz_prefix_t)prefix_num;
		for (i = 0;
		     ip_labels > 0 && i < DNS_RPZ_CIDR_WORDS * 2;
		     ip_labels--) {
			if (cp[0] == 'z' && cp[1] == 'z' &&
			    (cp[2] == '.' || cp[2] == '\0') &&
			    i <= 6) {
				do {
					if ((i & 1) == 0)
					    tgt_ip->w[3-i/2] = 0;
					++i;
				} while (ip_labels + i <= 8);
				cp += 3;
			} else {
				l = strtoul(cp, &cp2, 16);
				if (l > 0xffffu ||
				    (*cp2 != '.' && *cp2 != '\0')) {
					if (*cp2 == '.')
					    *cp2 = '\0';
					badname(log_level, src_name,
						"; invalid IPv6 word ", cp);
					return (ISC_R_FAILURE);
				}
				if ((i & 1) == 0)
					tgt_ip->w[3-i/2] = l;
				else
					tgt_ip->w[3-i/2] |= l << 16;
				i++;
				cp = cp2 + 1;
			}
		}
	}
	if (cp != end) {
		badname(log_level, src_name, "", "");
		return (ISC_R_FAILURE);
	}

	/*
	 * Check for 1s after the prefix length.
	 */
	prefix = (dns_rpz_prefix_t)prefix_num;
	while (prefix < DNS_RPZ_CIDR_KEY_BITS) {
		dns_rpz_cidr_word_t aword;

		i = prefix % DNS_RPZ_CIDR_WORD_BITS;
		aword = tgt_ip->w[prefix / DNS_RPZ_CIDR_WORD_BITS];
		if ((aword & ~DNS_RPZ_WORD_MASK(i)) != 0) {
			badname(log_level, src_name,
				"; too small prefix length of ", prefix_str);
			return (ISC_R_FAILURE);
		}
		prefix -= i;
		prefix += DNS_RPZ_CIDR_WORD_BITS;
	}

	/*
	 * Convert the address back to a canonical domain name
	 * to ensure that the original name is in canonical form.
	 */
	dns_fixedname_init(&ip_name2f);
	ip_name2 = dns_fixedname_name(&ip_name2f);
	result = ip2name(tgt_ip, (dns_rpz_prefix_t)prefix_num, NULL, ip_name2);
	if (result != ISC_R_SUCCESS || !dns_name_equal(&ip_name, ip_name2)) {
		badname(log_level, src_name, "; not canonical", "");
		return (ISC_R_FAILURE);
	}

	return (ISC_R_SUCCESS);
}

/*
 * Get trigger name and data bits for adding or deleting summary NSDNAME
 * or QNAME data.
 */
static void
name2data(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	  dns_rpz_type_t rpz_type, const dns_name_t *src_name,
	  dns_name_t *trig_name, dns_rpz_nm_data_t *new_data)
{
	static dns_rpz_pair_zbits_t zero = {0, 0};
	dns_rpz_zone_t *rpz;
	dns_offsets_t tmp_name_offsets;
	dns_name_t tmp_name;
	unsigned int prefix_len, n;

	REQUIRE(rpzs != NULL && rpz_num < rpzs->p.num_zones);
	rpz = rpzs->zones[rpz_num];
	REQUIRE(rpz != NULL);

	/*
	 * Handle wildcards by putting only the parent into the
	 * summary RBT.  The summary database only causes a check of the
	 * real policy zone where wildcards will be handled.
	 */
	if (dns_name_iswildcard(src_name)) {
		prefix_len = 1;
		new_data->pair = zero;
		make_pair(&new_data->wild, DNS_RPZ_ZBIT(rpz_num), rpz_type);
	} else {
		prefix_len = 0;
		make_pair(&new_data->pair, DNS_RPZ_ZBIT(rpz_num), rpz_type);
		new_data->wild = zero;
	}

	dns_name_init(&tmp_name, tmp_name_offsets);
	n = dns_name_countlabels(src_name);
	n -= prefix_len;
	if (rpz_type == DNS_RPZ_TYPE_QNAME)
		n -= dns_name_countlabels(&rpz->origin);
	else
		n -= dns_name_countlabels(&rpz->nsdname);
	dns_name_getlabelsequence(src_name, prefix_len, n, &tmp_name);
	(void)dns_name_concatenate(&tmp_name, dns_rootname, trig_name, NULL);
}

/*
 * Find the first differing bit in a key (IP address) word.
 */
static inline int
ffs_keybit(dns_rpz_cidr_word_t w) {
	int bit;

	bit = DNS_RPZ_CIDR_WORD_BITS-1;
	if ((w & 0xffff0000) != 0) {
		w >>= 16;
		bit -= 16;
	}
	if ((w & 0xff00) != 0) {
		w >>= 8;
		bit -= 8;
	}
	if ((w & 0xf0) != 0) {
		w >>= 4;
		bit -= 4;
	}
	if ((w & 0xc) != 0) {
		w >>= 2;
		bit -= 2;
	}
	if ((w & 2) != 0)
		--bit;
	return (bit);
}

/*
 * Find the first differing bit in two keys (IP addresses).
 */
static int
diff_keys(const dns_rpz_cidr_key_t *key1, dns_rpz_prefix_t prefix1,
	  const dns_rpz_cidr_key_t *key2, dns_rpz_prefix_t prefix2)
{
	dns_rpz_cidr_word_t delta;
	dns_rpz_prefix_t maxbit, bit;
	int i;

	maxbit = ISC_MIN(prefix1, prefix2);

	/*
	 * find the first differing words
	 */
	for (i = 0, bit = 0;
	     bit <= maxbit;
	     i++, bit += DNS_RPZ_CIDR_WORD_BITS) {
		delta = key1->w[i] ^ key2->w[i];
		if (delta != 0) {
			bit += ffs_keybit(delta);
			break;
		}
	}
	return (ISC_MIN(bit, maxbit));
}

/*
 * Given a hit while searching the radix trees,
 * clear all bits for higher numbered zones.
 */
static inline dns_rpz_zbits_t
trim_zbits(dns_rpz_zbits_t zbits, dns_rpz_zbits_t found) {
	dns_rpz_zbits_t x;

	/*
	 * Isolate the first or smallest numbered hit bit.
	 * Make a mask of that bit and all smaller numbered bits.
	 */
	x = zbits & found;
	x &= -x;
	x = (x << 1) - 1;
	return (zbits &= x);
}

/*
 * Search a radix tree for an IP address for ordinary lookup
 *	or for a CIDR block adding or deleting an entry
 *
 * Return ISC_R_SUCCESS, DNS_R_PARTIALMATCH, ISC_R_NOTFOUND,
 *	    and *found=longest match node
 *	or with create==ISC_TRUE, ISC_R_EXISTS or ISC_R_NOMEMORY
 */
static isc_result_t
search(dns_rpz_zones_t *rpzs,
       const dns_rpz_cidr_key_t *tgt_ip, dns_rpz_prefix_t tgt_prefix,
       const dns_rpz_pair_zbits_t *tgt_pair, isc_boolean_t create,
       dns_rpz_cidr_node_t **found)
{
	dns_rpz_cidr_node_t *cur, *parent, *child, *new_parent, *sibling;
	dns_rpz_pair_zbits_t pair;
	int cur_num, child_num;
	dns_rpz_prefix_t dbit;
	isc_result_t find_result;

	pair = *tgt_pair;
	find_result = ISC_R_NOTFOUND;
	*found = NULL;
	cur = rpzs->cidr;
	parent = NULL;
	cur_num = 0;
	for (;;) {
		if (cur == NULL) {
			/*
			 * No child so we cannot go down.
			 * Quit with whatever we already found
			 * or add the target as a child of the current parent.
			 */
			if (!create)
				return (find_result);
			child = new_node(rpzs, tgt_ip, tgt_prefix, NULL);
			if (child == NULL)
				return (ISC_R_NOMEMORY);
			if (parent == NULL)
				rpzs->cidr = child;
			else
				parent->child[cur_num] = child;
			child->parent = parent;
			child->pair.d |= tgt_pair->d;
			child->pair.ns |= tgt_pair->ns;
			set_sum_pair(child);
			*found = cur;
			return (ISC_R_SUCCESS);
		}

		if ((cur->sum.d & pair.d) == 0 &&
		    (cur->sum.ns & pair.ns) == 0) {
			/*
			 * This node has no relevant data
			 * and is in none of the target trees.
			 * Pretend it does not exist if we are not adding.
			 *
			 * If we are adding, continue down to eventually add
			 * a node and mark/put this node in the correct tree.
			 */
			if (!create)
				return (find_result);
		}

		dbit = diff_keys(tgt_ip, tgt_prefix, &cur->ip, cur->prefix);
		/*
		 * dbit <= tgt_prefix and dbit <= cur->prefix always.
		 * We are finished searching if we matched all of the target.
		 */
		if (dbit == tgt_prefix) {
			if (tgt_prefix == cur->prefix) {
				/*
				 * The node's key matches the target exactly.
				 */
				if ((cur->pair.d & pair.d) != 0 ||
				    (cur->pair.ns & pair.ns) != 0) {
					/*
					 * It is the answer if it has data.
					 */
					*found = cur;
					if (create) {
					    find_result = ISC_R_EXISTS;
					} else {
					    find_result = ISC_R_SUCCESS;
					}
				} else if (create) {
					/*
					 * The node lacked relevant data,
					 * but will have it now.
					 */
					cur->pair.d |= tgt_pair->d;
					cur->pair.ns |= tgt_pair->ns;
					set_sum_pair(cur);
					*found = cur;
					find_result = ISC_R_SUCCESS;
				}
				return (find_result);
			}

			/*
			 * We know tgt_prefix < cur->prefix which means that
			 * the target is shorter than the current node.
			 * Add the target as the current node's parent.
			 */
			if (!create)
				return (find_result);

			new_parent = new_node(rpzs, tgt_ip, tgt_prefix, cur);
			if (new_parent == NULL)
				return (ISC_R_NOMEMORY);
			new_parent->parent = parent;
			if (parent == NULL)
				rpzs->cidr = new_parent;
			else
				parent->child[cur_num] = new_parent;
			child_num = DNS_RPZ_IP_BIT(&cur->ip, tgt_prefix+1);
			new_parent->child[child_num] = cur;
			cur->parent = new_parent;
			new_parent->pair = *tgt_pair;
			set_sum_pair(new_parent);
			*found = new_parent;
			return (ISC_R_SUCCESS);
		}

		if (dbit == cur->prefix) {
			if ((cur->pair.d & pair.d) != 0 ||
			    (cur->pair.ns & pair.ns) != 0) {
				/*
				 * We have a partial match between of all of the
				 * current node but only part of the target.
				 * Continue searching for other hits in the
				 * same or lower numbered trees.
				 */
				find_result = DNS_R_PARTIALMATCH;
				*found = cur;
				pair.d = trim_zbits(pair.d, cur->pair.d);
				pair.ns = trim_zbits(pair.ns, cur->pair.ns);
			}
			parent = cur;
			cur_num = DNS_RPZ_IP_BIT(tgt_ip, dbit);
			cur = cur->child[cur_num];
			continue;
		}


		/*
		 * dbit < tgt_prefix and dbit < cur->prefix,
		 * so we failed to match both the target and the current node.
		 * Insert a fork of a parent above the current node and
		 * add the target as a sibling of the current node
		 */
		if (!create)
			return (find_result);

		sibling = new_node(rpzs, tgt_ip, tgt_prefix, NULL);
		if (sibling == NULL)
			return (ISC_R_NOMEMORY);
		new_parent = new_node(rpzs, tgt_ip, dbit, cur);
		if (new_parent == NULL) {
			isc_mem_put(rpzs->mctx, sibling, sizeof(*sibling));
			return (ISC_R_NOMEMORY);
		}
		new_parent->parent = parent;
		if (parent == NULL)
			rpzs->cidr = new_parent;
		else
			parent->child[cur_num] = new_parent;
		child_num = DNS_RPZ_IP_BIT(tgt_ip, dbit);
		new_parent->child[child_num] = sibling;
		new_parent->child[1-child_num] = cur;
		cur->parent = new_parent;
		sibling->parent = new_parent;
		sibling->pair = *tgt_pair;
		set_sum_pair(sibling);
		*found = sibling;
		return (ISC_R_SUCCESS);
	}
}

/*
 * Add an IP address to the radix tree.
 */
static isc_result_t
add_cidr(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	 dns_rpz_type_t rpz_type, dns_name_t *src_name)
{
	dns_rpz_cidr_key_t tgt_ip;
	dns_rpz_prefix_t tgt_prefix;
	dns_rpz_pair_zbits_t pair;
	dns_rpz_cidr_node_t *found;
	isc_result_t result;

	result = name2ipkey(DNS_RPZ_ERROR_LEVEL, rpzs, rpz_num, rpz_type,
			    src_name, &tgt_ip, &tgt_prefix, &pair);
	/*
	 * Log complaints about bad owner names but let the zone load.
	 */
	if (result != ISC_R_SUCCESS)
		return (ISC_R_SUCCESS);

	result = search(rpzs, &tgt_ip, tgt_prefix, &pair, ISC_TRUE, &found);
	if (result != ISC_R_SUCCESS) {
		char namebuf[DNS_NAME_FORMATSIZE];

		/*
		 * bin/tests/system/rpz/tests.sh looks for "rpz.*failed".
		 */
		dns_name_format(src_name, namebuf, sizeof(namebuf));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "rpz add_cidr(%s) failed: %s",
			      namebuf, isc_result_totext(result));
		return (result);
	}

	adj_trigger_cnt(rpzs, rpz_num, rpz_type, &tgt_ip, tgt_prefix, ISC_TRUE);
	return (result);
}

static isc_result_t
add_nm(dns_rpz_zones_t *rpzs, dns_name_t *trig_name,
	 const dns_rpz_nm_data_t *new_data)
{
	dns_rbtnode_t *nmnode;
	dns_rpz_nm_data_t *nm_data;
	isc_result_t result;

	nmnode = NULL;
	result = dns_rbt_addnode(rpzs->rbt, trig_name, &nmnode);
	switch (result) {
	case ISC_R_SUCCESS:
	case ISC_R_EXISTS:
		nm_data = nmnode->data;
		if (nm_data == NULL) {
			nm_data = isc_mem_get(rpzs->mctx, sizeof(*nm_data));
			if (nm_data == NULL)
				return (ISC_R_NOMEMORY);
			*nm_data = *new_data;
			nmnode->data = nm_data;
			return (ISC_R_SUCCESS);
		}
		break;
	default:
		return (result);
	}

	/*
	 * Do not count bits that are already present
	 */
	if ((nm_data->pair.d & new_data->pair.d) != 0 ||
	    (nm_data->pair.ns & new_data->pair.ns) != 0 ||
	    (nm_data->wild.d & new_data->wild.d) != 0 ||
	    (nm_data->wild.ns & new_data->wild.ns) != 0) {
		char namebuf[DNS_NAME_FORMATSIZE];

		/*
		 * bin/tests/system/rpz/tests.sh looks for "rpz.*failed".
		 */
		dns_name_format(trig_name, namebuf, sizeof(namebuf));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "rpz add_nm(%s): bits already set", namebuf);
		return (ISC_R_EXISTS);
	}

	nm_data->pair.d |= new_data->pair.d;
	nm_data->pair.ns |= new_data->pair.ns;
	nm_data->wild.d |= new_data->wild.d;
	nm_data->wild.ns |= new_data->wild.ns;
	return (ISC_R_SUCCESS);
}

static isc_result_t
add_name(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	 dns_rpz_type_t rpz_type, dns_name_t *src_name)
{
	dns_rpz_nm_data_t new_data;
	dns_fixedname_t trig_namef;
	dns_name_t *trig_name;
	isc_result_t result;

	dns_fixedname_init(&trig_namef);
	trig_name = dns_fixedname_name(&trig_namef);
	name2data(rpzs, rpz_num, rpz_type, src_name, trig_name, &new_data);

	result = add_nm(rpzs, trig_name, &new_data);
	if (result == ISC_R_SUCCESS)
		adj_trigger_cnt(rpzs, rpz_num, rpz_type, NULL, 0, ISC_TRUE);
	return (result);
}

/*
 * Callback to free the data for a node in the summary RBT database.
 */
static void
rpz_node_deleter(void *nm_data, void *mctx) {
	isc_mem_put(mctx, nm_data, sizeof(dns_rpz_nm_data_t));
}

/*
 * Get ready for a new set of policy zones.
 */
isc_result_t
dns_rpz_new_zones(dns_rpz_zones_t **rpzsp, isc_mem_t *mctx) {
	dns_rpz_zones_t *new;
	isc_result_t result;

	REQUIRE(rpzsp != NULL && *rpzsp == NULL);

	*rpzsp = NULL;

	new = isc_mem_get(mctx, sizeof(*new));
	if (new == NULL)
		return (ISC_R_NOMEMORY);
	memset(new, 0, sizeof(*new));

	result = isc_mutex_init(&new->search_lock);
	if (result != ISC_R_SUCCESS) {
		isc_mem_put(mctx, new, sizeof(*new));
		return (result);
	}

	result = isc_mutex_init(&new->maint_lock);
	if (result != ISC_R_SUCCESS) {
		DESTROYLOCK(&new->search_lock);
		isc_mem_put(mctx, new, sizeof(*new));
		return (result);
	}

	result = isc_refcount_init(&new->refs, 1);
	if (result != ISC_R_SUCCESS) {
		DESTROYLOCK(&new->maint_lock);
		DESTROYLOCK(&new->search_lock);
		isc_mem_put(mctx, new, sizeof(*new));
		return (result);
	}

	result = dns_rbt_create(mctx, rpz_node_deleter, mctx, &new->rbt);
	if (result != ISC_R_SUCCESS) {
		isc_refcount_decrement(&new->refs, NULL);
		isc_refcount_destroy(&new->refs);
		DESTROYLOCK(&new->maint_lock);
		DESTROYLOCK(&new->search_lock);
		isc_mem_put(mctx, new, sizeof(*new));
		return (result);
	}

	isc_mem_attach(mctx, &new->mctx);

	*rpzsp = new;
	return (ISC_R_SUCCESS);
}

/*
 * Free the radix tree of a response policy database.
 */
static void
cidr_free(dns_rpz_zones_t *rpzs) {
	dns_rpz_cidr_node_t *cur, *child, *parent;

	cur = rpzs->cidr;
	while (cur != NULL) {
		/* Depth first. */
		child = cur->child[0];
		if (child != NULL) {
			cur = child;
			continue;
		}
		child = cur->child[1];
		if (child != NULL) {
			cur = child;
			continue;
		}

		/* Delete this leaf and go up. */
		parent = cur->parent;
		if (parent == NULL)
			rpzs->cidr = NULL;
		else
			parent->child[parent->child[1] == cur] = NULL;
		isc_mem_put(rpzs->mctx, cur, sizeof(*cur));
		cur = parent;
	}
}

/*
 * Discard a response policy zone blob
 * before discarding the overall rpz structure.
 */
static void
rpz_detach(dns_rpz_zone_t **rpzp, dns_rpz_zones_t *rpzs) {
	dns_rpz_zone_t *rpz;
	unsigned int refs;

	rpz = *rpzp;
	*rpzp = NULL;
	isc_refcount_decrement(&rpz->refs, &refs);
	if (refs != 0)
		return;
	isc_refcount_destroy(&rpz->refs);

	if (dns_name_dynamic(&rpz->origin))
		dns_name_free(&rpz->origin, rpzs->mctx);
	if (dns_name_dynamic(&rpz->ip))
		dns_name_free(&rpz->ip, rpzs->mctx);
	if (dns_name_dynamic(&rpz->nsdname))
		dns_name_free(&rpz->nsdname, rpzs->mctx);
	if (dns_name_dynamic(&rpz->nsip))
		dns_name_free(&rpz->nsip, rpzs->mctx);
	if (dns_name_dynamic(&rpz->passthru))
		dns_name_free(&rpz->passthru, rpzs->mctx);
	if (dns_name_dynamic(&rpz->cname))
		dns_name_free(&rpz->cname, rpzs->mctx);

	isc_mem_put(rpzs->mctx, rpz, sizeof(*rpz));
}

void
dns_rpz_attach_rpzs(dns_rpz_zones_t *rpzs, dns_rpz_zones_t **rpzsp) {
	REQUIRE(rpzsp != NULL && *rpzsp == NULL);
	isc_refcount_increment(&rpzs->refs, NULL);
	*rpzsp = rpzs;
}

/*
 * Forget a view's policy zones.
 */
void
dns_rpz_detach_rpzs(dns_rpz_zones_t **rpzsp) {
	dns_rpz_zones_t *rpzs;
	dns_rpz_zone_t *rpz;
	dns_rpz_num_t rpz_num;
	unsigned int refs;

	REQUIRE(rpzsp != NULL);
	rpzs = *rpzsp;
	REQUIRE(rpzs != NULL);

	*rpzsp = NULL;
	isc_refcount_decrement(&rpzs->refs, &refs);

	/*
	 * Forget the last of view's rpz machinery after the last reference.
	 */
	if (refs == 0) {
		for (rpz_num = 0; rpz_num < DNS_RPZ_MAX_ZONES; ++rpz_num) {
			rpz = rpzs->zones[rpz_num];
			rpzs->zones[rpz_num] = NULL;
			if (rpz != NULL)
				rpz_detach(&rpz, rpzs);
		}

		cidr_free(rpzs);
		dns_rbt_destroy(&rpzs->rbt);
		DESTROYLOCK(&rpzs->maint_lock);
		DESTROYLOCK(&rpzs->search_lock);
		isc_refcount_destroy(&rpzs->refs);
		isc_mem_putanddetach(&rpzs->mctx, rpzs, sizeof(*rpzs));
	}
}

/*
 * Create empty summary database to load one zone.
 * The RBTDB write tree lock must be held.
 */
isc_result_t
dns_rpz_beginload(dns_rpz_zones_t **load_rpzsp,
		  dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num)
{
	dns_rpz_zones_t *load_rpzs;
	dns_rpz_zone_t *rpz;
	dns_rpz_zbits_t tgt;
	isc_result_t result;

	REQUIRE(rpz_num < rpzs->p.num_zones);
	rpz = rpzs->zones[rpz_num];
	REQUIRE(rpz != NULL);

	/*
	 * When reloading a zone, there are usually records among the summary
	 * data for the zone.  Some of those records might be deleted by the
	 * reloaded zone data.  To deal with that case:
	 *    reload the new zone data into a new blank summary database
	 *    if the reload fails, discard the new summary database
	 *    if the new zone data is acceptable, copy the records for the
	 *	other zones into the new summary database and replace the
	 *	old summary database with the new.
	 *
	 * At the first attempt to load a zone, there is no summary data
	 * for the zone and so no records that need to be deleted.
	 * This is also the most common case of policy zone loading.
	 * Most policy zone maintenance should be by incremental changes
	 * and so by the addition and deletion of individual records.
	 * Detect that case and load records the first time into the
	 * operational summary database
	 */
	tgt = DNS_RPZ_ZBIT(rpz_num);
	LOCK(&rpzs->maint_lock);
	LOCK(&rpzs->search_lock);
	if ((rpzs->load_begun & tgt) == 0) {
		rpzs->load_begun |= tgt;
		dns_rpz_attach_rpzs(rpzs, load_rpzsp);
		UNLOCK(&rpzs->search_lock);
		UNLOCK(&rpzs->maint_lock);

	} else {
		UNLOCK(&rpzs->search_lock);
		UNLOCK(&rpzs->maint_lock);

		result = dns_rpz_new_zones(load_rpzsp, rpzs->mctx);
		if (result != ISC_R_SUCCESS)
			return (result);
		load_rpzs = *load_rpzsp;
		load_rpzs->p.num_zones = rpzs->p.num_zones;
		load_rpzs->zones[rpz_num] = rpz;
		isc_refcount_increment(&rpz->refs, NULL);
	}

	return (ISC_R_SUCCESS);
}

static void
fix_triggers(dns_rpz_zones_t *rpzs, dns_rpz_triggers_t *totals) {
	dns_rpz_num_t rpz_num;
	const dns_rpz_zone_t *rpz;
	dns_rpz_zbits_t zbit;

#	define SET_TRIG(type)						\
	if (rpz == NULL || rpz->triggers.type == 0) {			\
		rpzs->have.type &= ~zbit;				\
	} else {							\
		totals->type += rpz->triggers.type;			\
		rpzs->have.type |= zbit;				\
	}

	memset(totals, 0, sizeof(*totals));
	for (rpz_num = 0; rpz_num < rpzs->p.num_zones; ++rpz_num) {
		rpz = rpzs->zones[rpz_num];
		zbit = DNS_RPZ_ZBIT(rpz_num);
		SET_TRIG(nsdname);
		SET_TRIG(qname);
		SET_TRIG(ipv4);
		SET_TRIG(ipv6);
		SET_TRIG(nsipv4);
		SET_TRIG(nsipv6);
	}

	fix_qname_skip_recurse(rpzs);

#	undef SET_TRIG
}

static void
load_unlock(dns_rpz_zones_t *rpzs, dns_rpz_zones_t **load_rpzsp) {
	UNLOCK(&rpzs->maint_lock);
	UNLOCK(&(*load_rpzsp)->search_lock);
	UNLOCK(&(*load_rpzsp)->maint_lock);
	dns_rpz_detach_rpzs(load_rpzsp);
}

/*
 * Finish loading one zone.
 * The RBTDB write tree lock must be held.
 */
isc_result_t
dns_rpz_ready(dns_rpz_zones_t *rpzs,
	      dns_rpz_zones_t **load_rpzsp, dns_rpz_num_t rpz_num)
{
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_rpz_zones_t *load_rpzs;
	const dns_rpz_cidr_node_t *cnode, *next_cnode, *parent_cnode;
	dns_rpz_cidr_node_t *found;
	dns_rpz_pair_zbits_t load_pair, new_pair;
	dns_rbt_t *rbt;
	dns_rbtnodechain_t chain;
	dns_rbtnode_t *nmnode;
	dns_rpz_nm_data_t *nm_data, new_data;
	dns_rpz_triggers_t old_totals, new_totals;
	dns_fixedname_t labelf, originf, namef;
	dns_name_t *label, *origin, *name;
	isc_result_t result;

	INSIST(rpzs != NULL);
	LOCK(&rpzs->maint_lock);
	load_rpzs = *load_rpzsp;
	INSIST(load_rpzs != NULL);

	if (load_rpzs == rpzs) {
		/*
		 * This is a successful initial zone loading,
		 * perhaps for a new instance of a view.
		 */
		fix_triggers(rpzs, &new_totals);
		UNLOCK(&rpzs->maint_lock);
		dns_rpz_detach_rpzs(load_rpzsp);

		if (rpz_num == rpzs->p.num_zones-1)
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
				      DNS_LOGMODULE_RBTDB, DNS_RPZ_INFO_LEVEL,
				      "loaded policy %d zones with"
				      " %d qname  %d nsdname"
				      "  % d IP  %d NSIP entries",
				      rpzs->p.num_zones,
				      new_totals.qname,
				      new_totals.nsdname,
				      new_totals.ipv4 + new_totals.ipv6,
				      new_totals.nsipv4 +
				      new_totals.nsipv6);
		return (ISC_R_SUCCESS);
	}

	LOCK(&load_rpzs->maint_lock);
	LOCK(&load_rpzs->search_lock);

	/*
	 * Copy the other policy zones to the new summary databases
	 * unless there is only one policy zone.
	 */
	if (rpzs->p.num_zones > 1) {
		/*
		 * Copy to the radix tree.
		 */
		load_pair.d = ~DNS_RPZ_ZBIT(rpz_num);
		load_pair.ns = load_pair.d;
		for (cnode = rpzs->cidr; cnode != NULL; cnode = next_cnode) {
			new_pair.d = cnode->pair.d & load_pair.d;
			new_pair.ns = cnode->pair.ns & load_pair.ns;
			if (new_pair.d != 0 || new_pair.ns != 0) {
				result = search(load_rpzs,
						&cnode->ip, cnode->prefix,
						&new_pair, ISC_TRUE,
						&found);
				if (result == ISC_R_NOMEMORY) {
					load_unlock(rpzs, load_rpzsp);
					return (result);
				}
				INSIST(result == ISC_R_SUCCESS);
			}
			/*
			 * Do down and to the left as far as possible.
			 */
			next_cnode = cnode->child[0];
			if (next_cnode != NULL)
				continue;
			/*
			 * Go up until we find a branch to the right where
			 * we previously took the branck to the left.
			 */
			for (;;) {
				parent_cnode = cnode->parent;
				if (parent_cnode == NULL)
					break;
				if (parent_cnode->child[0] == cnode) {
					next_cnode = parent_cnode->child[1];
					if (next_cnode != NULL)
					    break;
				}
				cnode = parent_cnode;
			}
		}

		/*
		 * Copy to the summary rbt.
		 */
		dns_fixedname_init(&namef);
		name = dns_fixedname_name(&namef);
		dns_fixedname_init(&labelf);
		label = dns_fixedname_name(&labelf);
		dns_fixedname_init(&originf);
		origin = dns_fixedname_name(&originf);
		dns_rbtnodechain_init(&chain, NULL);
		result = dns_rbtnodechain_first(&chain, rpzs->rbt, NULL, NULL);
		while (result == DNS_R_NEWORIGIN || result == ISC_R_SUCCESS) {
			result = dns_rbtnodechain_current(&chain, label, origin,
							&nmnode);
			INSIST(result == ISC_R_SUCCESS);
			nm_data = nmnode->data;
			if (nm_data != NULL) {
				new_data.pair.d = (nm_data->pair.d &
						   load_pair.d);
				new_data.pair.ns = (nm_data->pair.ns &
						   load_pair.ns);
				new_data.wild.d = (nm_data->wild.d &
						   load_pair.d);
				new_data.wild.ns = (nm_data->wild.ns &
						   load_pair.ns);
				if (new_data.pair.d != 0 ||
				    new_data.pair.ns != 0 ||
				    new_data.wild.d != 0 ||
				    new_data.wild.ns != 0) {
					result = dns_name_concatenate(label,
							origin, name, NULL);
					INSIST(result == ISC_R_SUCCESS);
					result = add_nm(load_rpzs, name,
							&new_data);
					if (result != ISC_R_SUCCESS) {
					    load_unlock(rpzs, load_rpzsp);
					    return (result);
					}
				}
			}
			result = dns_rbtnodechain_next(&chain, NULL, NULL);
		}
		if (result != ISC_R_NOMORE && result != ISC_R_NOTFOUND) {
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
				      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
				      "dns_rpz_ready(): unexpected %s",
				      isc_result_totext(result));
			load_unlock(rpzs, load_rpzsp);
			return (result);
		}
	}

	fix_triggers(rpzs, &old_totals);
	fix_triggers(load_rpzs, &new_totals);

	dns_name_format(&load_rpzs->zones[rpz_num]->origin,
			namebuf, sizeof(namebuf));
	isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
		      DNS_LOGMODULE_RBTDB, DNS_RPZ_INFO_LEVEL,
		      "reloading policy zone '%s' changed from"
		      " %d to %d qname, %d to %d nsdname,"
		      " %d to %d IP, %d to %d NSIP entries",
		      namebuf,
		      old_totals.qname, new_totals.qname,
		      old_totals.nsdname, new_totals.nsdname,
		      old_totals.ipv4 + old_totals.ipv6,
		      new_totals.ipv4 + new_totals.ipv6,
		      old_totals.nsipv4 + old_totals.nsipv6,
		      new_totals.nsipv4 + new_totals.nsipv6);

	/*
	 * Exchange the summary databases.
	 */
	LOCK(&rpzs->search_lock);

	found = rpzs->cidr;
	rpzs->cidr = load_rpzs->cidr;
	load_rpzs->cidr = found;

	rbt = rpzs->rbt;
	rpzs->rbt = load_rpzs->rbt;
	load_rpzs->rbt = rbt;

	UNLOCK(&rpzs->search_lock);
	load_unlock(rpzs, load_rpzsp);
	return (ISC_R_SUCCESS);
}

/*
 * Add an IP address to the radix tree or a name to the summary database.
 */
isc_result_t
dns_rpz_add(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num, dns_name_t *src_name)
{
	dns_rpz_zone_t *rpz;
	dns_rpz_type_t rpz_type;
	isc_result_t result = ISC_R_FAILURE;

	REQUIRE(rpzs != NULL && rpz_num < rpzs->p.num_zones);
	rpz = rpzs->zones[rpz_num];
	REQUIRE(rpz != NULL);

	rpz_type = type_from_name(rpz, src_name);

	LOCK(&rpzs->maint_lock);
	LOCK(&rpzs->search_lock);

	switch (rpz_type) {
	case DNS_RPZ_TYPE_QNAME:
	case DNS_RPZ_TYPE_NSDNAME:
		result = add_name(rpzs, rpz_num, rpz_type, src_name);
		break;
	case DNS_RPZ_TYPE_IP:
	case DNS_RPZ_TYPE_NSIP:
		result = add_cidr(rpzs, rpz_num, rpz_type, src_name);
		break;
	case DNS_RPZ_TYPE_BAD:
		break;
	}

	UNLOCK(&rpzs->search_lock);
	UNLOCK(&rpzs->maint_lock);
	return (result);
}

/*
 * Remove an IP address from the radix tree.
 */
static void
del_cidr(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	 dns_rpz_type_t rpz_type, dns_name_t *src_name)
{
	isc_result_t result;
	dns_rpz_cidr_key_t tgt_ip;
	dns_rpz_prefix_t tgt_prefix;
	dns_rpz_pair_zbits_t pair;
	dns_rpz_cidr_node_t *tgt, *parent, *child;

	/*
	 * Do not worry about invalid rpz IP address names.  If we
	 * are here, then something relevant was added and so was
	 * valid.  Invalid names here are usually internal RBTDB nodes.
	 */
	result = name2ipkey(DNS_RPZ_DEBUG_QUIET, rpzs, rpz_num, rpz_type,
			    src_name, &tgt_ip, &tgt_prefix, &pair);
	if (result != ISC_R_SUCCESS)
		return;

	result = search(rpzs, &tgt_ip, tgt_prefix, &pair, ISC_FALSE, &tgt);
	if (result != ISC_R_SUCCESS) {
		INSIST(result == ISC_R_NOTFOUND ||
		       result == DNS_R_PARTIALMATCH);
		/*
		 * Do not worry about missing summary RBT nodes that probably
		 * correspond to RBTDB nodes that were implicit RBT nodes
		 * that were later added for (often empty) wildcards
		 * and then to the RBTDB deferred cleanup list.
		 */
		return;
	}

	/*
	 * Mark the node and its parents to reflect the deleted IP address.
	 * Do not count bits that are already clear for internal RBTDB nodes.
	 */
	pair.d &= tgt->pair.d;
	pair.ns &= tgt->pair.ns;
	tgt->pair.d &= ~pair.d;
	tgt->pair.ns &= ~pair.ns;
	set_sum_pair(tgt);

	adj_trigger_cnt(rpzs, rpz_num, rpz_type, &tgt_ip, tgt_prefix, ISC_FALSE);

	/*
	 * We might need to delete 2 nodes.
	 */
	do {
		/*
		 * The node is now useless if it has no data of its own
		 * and 0 or 1 children.  We are finished if it is not useless.
		 */
		if ((child = tgt->child[0]) != NULL) {
			if (tgt->child[1] != NULL)
				break;
		} else {
			child = tgt->child[1];
		}
		if (tgt->pair.d + tgt->pair.ns != 0)
			break;

		/*
		 * Replace the pointer to this node in the parent with
		 * the remaining child or NULL.
		 */
		parent = tgt->parent;
		if (parent == NULL) {
			rpzs->cidr = child;
		} else {
			parent->child[parent->child[1] == tgt] = child;
		}
		/*
		 * If the child exists fix up its parent pointer.
		 */
		if (child != NULL)
			child->parent = parent;
		isc_mem_put(rpzs->mctx, tgt, sizeof(*tgt));

		tgt = parent;
	} while (tgt != NULL);
}

static void
del_name(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	 dns_rpz_type_t rpz_type, dns_name_t *src_name)
{
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_fixedname_t trig_namef;
	dns_name_t *trig_name;
	dns_rbtnode_t *nmnode;
	dns_rpz_nm_data_t *nm_data, del_data;
	isc_result_t result;

	dns_fixedname_init(&trig_namef);
	trig_name = dns_fixedname_name(&trig_namef);
	name2data(rpzs, rpz_num, rpz_type, src_name, trig_name, &del_data);

	/*
	 * No need for a summary database of names with only 1 policy zone.
	 */
	if (rpzs->p.num_zones <= 1) {
		adj_trigger_cnt(rpzs, rpz_num, rpz_type, NULL, 0, ISC_FALSE);
		return;
	}

	nmnode = NULL;
	result = dns_rbt_findnode(rpzs->rbt, trig_name, NULL, &nmnode, NULL, 0,
				  NULL, NULL);
	if (result != ISC_R_SUCCESS) {
		/*
		 * Do not worry about missing summary RBT nodes that probably
		 * correspond to RBTDB nodes that were implicit RBT nodes
		 * that were later added for (often empty) wildcards
		 * and then to the RBTDB deferred cleanup list.
		 */
		if (result == ISC_R_NOTFOUND)
			return;
		dns_name_format(src_name, namebuf, sizeof(namebuf));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "rpz del_name(%s) node search failed: %s",
			      namebuf, isc_result_totext(result));
		return;
	}

	nm_data = nmnode->data;
	INSIST(nm_data != NULL);

	/*
	 * Do not count bits that next existed for RBT nodes that would we
	 * would not have found in a summary for a single RBTDB tree.
	 */
	del_data.pair.d &= nm_data->pair.d;
	del_data.pair.ns &= nm_data->pair.ns;
	del_data.wild.d &= nm_data->wild.d;
	del_data.wild.ns &= nm_data->wild.ns;

	nm_data->pair.d &= ~del_data.pair.d;
	nm_data->pair.ns &= ~del_data.pair.ns;
	nm_data->wild.d &= ~del_data.wild.d;
	nm_data->wild.ns &= ~del_data.wild.ns;

	if (nm_data->pair.d == 0 && nm_data->pair.ns == 0 &&
	    nm_data->wild.d == 0 && nm_data->wild.ns == 0) {
		result = dns_rbt_deletenode(rpzs->rbt, nmnode, ISC_FALSE);
		if (result != ISC_R_SUCCESS) {
			/*
			 * bin/tests/system/rpz/tests.sh looks for "rpz.*failed".
			 */
			dns_name_format(src_name, namebuf, sizeof(namebuf));
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
				      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
				      "rpz del_name(%s) node delete failed: %s",
				      namebuf, isc_result_totext(result));
		}
	}

	adj_trigger_cnt(rpzs, rpz_num, rpz_type, NULL, 0, ISC_FALSE);
}

/*
 * Remove an IP address from the radix tree or a name from the summary database.
 */
void
dns_rpz_delete(dns_rpz_zones_t *rpzs, dns_rpz_num_t rpz_num,
	       dns_name_t *src_name) {
	dns_rpz_zone_t *rpz;
	dns_rpz_type_t rpz_type;

	REQUIRE(rpzs != NULL && rpz_num < rpzs->p.num_zones);
	rpz = rpzs->zones[rpz_num];
	REQUIRE(rpz != NULL);

	rpz_type = type_from_name(rpz, src_name);

	LOCK(&rpzs->maint_lock);
	LOCK(&rpzs->search_lock);

	switch (rpz_type) {
	case DNS_RPZ_TYPE_QNAME:
	case DNS_RPZ_TYPE_NSDNAME:
		del_name(rpzs, rpz_num, rpz_type, src_name);
		break;
	case DNS_RPZ_TYPE_IP:
	case DNS_RPZ_TYPE_NSIP:
		del_cidr(rpzs, rpz_num, rpz_type, src_name);
		break;
	case DNS_RPZ_TYPE_BAD:
		break;
	}

	UNLOCK(&rpzs->search_lock);
	UNLOCK(&rpzs->maint_lock);
}

/*
 * Search the summary radix tree to get a relative owner name in a
 * policy zone relevant to a triggering IP address.
 *	rpz_type and zbits limit the search for IP address netaddr
 *	return the policy zone's number or DNS_RPZ_INVALID_NUM
 *	ip_name is the relative owner name found and
 *	*prefixp is its prefix length.
 */
dns_rpz_num_t
dns_rpz_find_ip(dns_rpz_zones_t *rpzs, dns_rpz_type_t rpz_type,
		dns_rpz_zbits_t zbits, const isc_netaddr_t *netaddr,
		dns_name_t *ip_name, dns_rpz_prefix_t *prefixp)
{
	dns_rpz_cidr_key_t tgt_ip;
	dns_rpz_pair_zbits_t pair;
	dns_rpz_cidr_node_t *found;
	isc_result_t result;
	dns_rpz_num_t rpz_num;
	int i;

	/*
	 * Convert IP address to CIDR tree key.
	 */
	if (netaddr->family == AF_INET) {
		tgt_ip.w[0] = 0;
		tgt_ip.w[1] = 0;
		tgt_ip.w[2] = ADDR_V4MAPPED;
		tgt_ip.w[3] = ntohl(netaddr->type.in.s_addr);
		if (rpz_type == DNS_RPZ_TYPE_IP)
			zbits &= rpzs->have.ipv4;
		else
			zbits &= rpzs->have.nsipv4;
	} else if (netaddr->family == AF_INET6) {
		dns_rpz_cidr_key_t src_ip6;

		/*
		 * Given the int aligned struct in_addr member of netaddr->type
		 * one could cast netaddr->type.in6 to dns_rpz_cidr_key_t *,
		 * but some people object.
		 */
		memcpy(src_ip6.w, &netaddr->type.in6, sizeof(src_ip6.w));
		for (i = 0; i < 4; i++) {
			tgt_ip.w[i] = ntohl(src_ip6.w[i]);
		}
		if (rpz_type == DNS_RPZ_TYPE_IP)
			zbits &= rpzs->have.ipv6;
		else
			zbits &= rpzs->have.nsipv6;
	} else {
		return (DNS_RPZ_INVALID_NUM);
	}

	if (zbits == 0)
		return (DNS_RPZ_INVALID_NUM);
	make_pair(&pair, zbits, rpz_type);

	LOCK(&rpzs->search_lock);
	result = search(rpzs, &tgt_ip, 128, &pair, ISC_FALSE, &found);
	if (result == ISC_R_NOTFOUND) {
		/*
		 * There are no eligible zones for this IP address.
		 */
		UNLOCK(&rpzs->search_lock);
		return (DNS_RPZ_INVALID_NUM);
	}

	/*
	 * Construct the trigger name for the longest matching trigger
	 * in the first eligible zone with a match.
	 */
	*prefixp = found->prefix;
	if (rpz_type == DNS_RPZ_TYPE_IP) {
		INSIST((found->pair.d & pair.d) != 0);
		rpz_num = zbit_to_num(found->pair.d & pair.d);
	} else {
		INSIST((found->pair.ns & pair.ns) != 0);
		rpz_num = zbit_to_num(found->pair.ns & pair.ns);
	}
	result = ip2name(&found->ip, found->prefix, dns_rootname, ip_name);
	UNLOCK(&rpzs->search_lock);
	if (result != ISC_R_SUCCESS) {
		/*
		 * bin/tests/system/rpz/tests.sh looks for "rpz.*failed".
		 */
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "rpz ip2name() failed: %s",
			      isc_result_totext(result));
		return (DNS_RPZ_INVALID_NUM);
	}
	return (rpz_num);
}

/*
 * Search the summary radix tree for policy zones with triggers matching
 * a name.
 */
dns_rpz_zbits_t
dns_rpz_find_name(dns_rpz_zones_t *rpzs, dns_rpz_type_t rpz_type,
		  dns_rpz_zbits_t zbits, dns_name_t *trig_name)
{
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_rbtnode_t *nmnode;
	const dns_rpz_nm_data_t *nm_data;
	dns_rpz_zbits_t found_zbits;
	isc_result_t result;

	if (zbits == 0)
		return (0);

	found_zbits = 0;

	LOCK(&rpzs->search_lock);

	nmnode = NULL;
	result = dns_rbt_findnode(rpzs->rbt, trig_name, NULL, &nmnode, NULL,
				  DNS_RBTFIND_EMPTYDATA, NULL, NULL);
	switch (result) {
	case ISC_R_SUCCESS:
		nm_data = nmnode->data;
		if (nm_data != NULL) {
			if (rpz_type == DNS_RPZ_TYPE_QNAME)
				found_zbits = nm_data->pair.d;
			else
				found_zbits = nm_data->pair.ns;
		}
		nmnode = nmnode->parent;
		/* fall thru */
	case DNS_R_PARTIALMATCH:
		while (nmnode != NULL) {
			nm_data = nmnode->data;
			if (nm_data != NULL) {
				if (rpz_type == DNS_RPZ_TYPE_QNAME)
					found_zbits |= nm_data->wild.d;
				else
					found_zbits |= nm_data->wild.ns;
			}
			nmnode = nmnode->parent;
		}
		break;

	case ISC_R_NOTFOUND:
		break;

	default:
		/*
		 * bin/tests/system/rpz/tests.sh looks for "rpz.*failed".
		 */
		dns_name_format(trig_name, namebuf, sizeof(namebuf));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_RPZ,
			      DNS_LOGMODULE_RBTDB, DNS_RPZ_ERROR_LEVEL,
			      "dns_rpz_find_name(%s) failed: %s",
			      namebuf, isc_result_totext(result));
		break;
	}

	UNLOCK(&rpzs->search_lock);
	return (zbits & found_zbits);
}

/*
 * Translate CNAME rdata to a QNAME response policy action.
 */
dns_rpz_policy_t
dns_rpz_decode_cname(dns_rpz_zone_t *rpz, dns_rdataset_t *rdataset,
		     dns_name_t *selfname)
{
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_cname_t cname;
	isc_result_t result;

	result = dns_rdataset_first(rdataset);
	INSIST(result == ISC_R_SUCCESS);
	dns_rdataset_current(rdataset, &rdata);
	result = dns_rdata_tostruct(&rdata, &cname, NULL);
	INSIST(result == ISC_R_SUCCESS);
	dns_rdata_reset(&rdata);

	/*
	 * CNAME . means NXDOMAIN
	 */
	if (dns_name_equal(&cname.cname, dns_rootname))
		return (DNS_RPZ_POLICY_NXDOMAIN);

	if (dns_name_iswildcard(&cname.cname)) {
		/*
		 * CNAME *. means NODATA
		 */
		if (dns_name_countlabels(&cname.cname) == 2)
			return (DNS_RPZ_POLICY_NODATA);

		/*
		 * A qname of www.evil.com and a policy of
		 *	*.evil.com    CNAME   *.garden.net
		 * gives a result of
		 *	evil.com    CNAME   evil.com.garden.net
		 */
		if (dns_name_countlabels(&cname.cname) > 2)
			return (DNS_RPZ_POLICY_WILDCNAME);
	}

	/*
	 * CNAME PASSTHRU. means "do not rewrite.
	 */
	if (dns_name_equal(&cname.cname, &rpz->passthru))
		return (DNS_RPZ_POLICY_PASSTHRU);

	/*
	 * 128.1.0.127.rpz-ip CNAME  128.1.0.0.127. is obsolete PASSTHRU
	 */
	if (selfname != NULL && dns_name_equal(&cname.cname, selfname))
		return (DNS_RPZ_POLICY_PASSTHRU);

	/*
	 * Any other rdata gives a response consisting of the rdata.
	 */
	return (DNS_RPZ_POLICY_RECORD);
}
