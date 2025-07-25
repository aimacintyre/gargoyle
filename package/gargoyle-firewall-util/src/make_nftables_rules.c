/*  make_nftables_rules --	A tool to generate firewall rules from options in UCI config files
 *  				Originally designed for use with Gargoyle router firmware (gargoyle-router.com)
 *
 *
 *  Copyright � 2009 by Eric Bishop <eric@gargoyle-router.com>
 *
 *  This file is free software: you may copy, redistribute and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation, either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This file is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <arpa/inet.h>

#include <math.h>

#include <erics_tools.h>
#include <uci.h>
#define malloc safe_malloc
#define strdup safe_strdup

/* these are indices don't change! */
#define MATCH_IP_INDEX 0
#define MATCH_IP6_INDEX 1
#define MATCH_MAC_INDEX 2

string_map* get_rule_definition(char* config, char* section);
char* get_option_value_string(struct uci_option* uopt);
int parse_option(char* option_name, char* option_value, string_map* definition);

char*** parse_ips_and_macs(char* addr_str);
char** parse_ports(char* port_str);
char** parse_marks(char* list_str, unsigned long max_mask);
list* parse_quoted_list(char* list_str, char quote_char, char escape_char, char add_remainder_if_uneven_quotes);

int truncate_if_starts_with(char* test_str, char* prefix);

char* invert_bitmask(const char* input);

char** compute_rules(string_map *rule_def, char* table, char* chain, int is_ingress, char* target, char* target_options);
int compute_multi_rules(char** def, list* multi_rules, char** single_check, int never_single, char* rule_prefix, char* test_prefix1, char* test_prefix2, int is_negation1, int is_negation2, int mask_byte_index, char* proto, int requires_proto, int quoted_args);

int main(int argc, char **argv)
{
	int c;
	char* package		= NULL;
	char* section		= NULL;
	char* table		= NULL;
	char* chain		= NULL;
	char* target		= NULL;
	char* target_options	= NULL;
	int is_ingress		= 0;
	int run_commands	= 0;
	int usage_printed	= 0;
	while((c = getopt(argc, argv, "P:p:S:s:T:t:C:c:G:g:O:o:IiRrUu")) != -1) //section, page, css includes, javascript includes, title, output interface variables
	{
		switch(c)
		{
			case 'P':
			case 'p':
				package = strdup(optarg);
				break;
			case 'S':
			case 's':
				section = strdup(optarg);
				break;
			case 'T':
			case 't':
				table = strdup(optarg);
				break;
			case 'C':
			case 'c':
				chain = strdup(optarg);
				break;
			case 'G':
			case 'g':
				target = strdup(optarg);
				break;
			case 'O':
			case 'o':
				target_options = strdup(optarg);
				break;
			case 'I':
			case 'i':
				is_ingress = 1;
				break;
			case 'R':
			case 'r':
				run_commands = 1;
				break;
			case 'U':
			case 'u':
			default:
				fprintf(stderr, "USAGE: %s -p [PACKAGE] -s [SECTION] -t [TABLE] -c [CHAIN] -g [TARGET] [OPTIONS]\n       -o [TARGET_OPTIONS]\n       -i indicates that this rule applies to ingress packets\n       -r implies computed commands should be executed instead of just printed\n       -u print usage and exit\n\n", argv[0]);
				usage_printed = 1;
				break;

		}
	}
	if(package != NULL && section != NULL && table != NULL && chain != NULL && target != NULL)
	{
		string_map* def = get_rule_definition(package, section);
		if(def !=  NULL)
		{
			char** rules = compute_rules(def, table, chain, 0, target, target_options);

			int rindex = 0;
			for(rindex=0; rules[rindex] != NULL; rindex++)
			{	
				if(run_commands == 0)
				{
					printf("%s\n", dynamic_strcat(2, "nft", rules[rindex]));
				}
				else
				{
					system(dynamic_strcat(2, "nft", rules[rindex]));	
				}
			}
		}
		else
		{
			fprintf(stderr, "ERROR: Invalid package / section\n");
		}
	}
	else if(!usage_printed)
	{
		fprintf(stderr, "USAGE: %s -p [PACKAGE] -s [SECTION] -t [TABLE] -c [CHAIN] -g [TARGET] [OPTIONS]\n       -o [TARGET_OPTIONS]\n       -i indicates that this rule applies to ingress packets\n       -r implies computed commands should be executed instead of just printed\n       -u print usage and exit\n\n", argv[0]);
	}

	return 0;
}

/* 
 * Note we've currently maxed out out one whole byte of address space
 * in the connmark at this point.  If we want to match in
 * further dimensions, we will have to be greedy and take 
 * even more address space
 */
char** compute_rules(string_map *rule_def, char* table, char* chain, int is_ingress, char* target, char* target_options)
{
	list* multi_rules = initialize_list();
	char* single_check = strdup("");
	char* rule_prefix = dynamic_strcat(5, " add rule ", table, " ", chain, " ");

	target = strdup(target);
	to_lowercase(target);
	
	/* check family */
	char* family = get_map_element(rule_def, "family");
	if(family == NULL)
	{
		family = strdup("any");
		set_map_element(rule_def, "family", family);
	}
	to_lowercase(family);
	if(safe_strcmp(family, "ipv4") != 0 && safe_strcmp(family, "ipv6") != 0 && safe_strcmp(family, "any") != 0)
	{
		char* tmp;
		tmp = set_map_element(rule_def, "family", strdup("any"));
		free(tmp);
		family = (char*)get_map_element(rule_def, "family");
	}
	
	/* apply family prefix */
	if(safe_strcmp(family, "any") != 0)
	{
		char* tmp;
		tmp = dynamic_strcat(4, rule_prefix, "meta nfproto ", family, " ");
		free(rule_prefix);
		rule_prefix = tmp;
	}

	/* get timerange vars first */
	char* active_hours = get_map_element(rule_def, "active_hours");
	char* active_weekdays  = get_map_element(rule_def, "active_weekdays");
	char* active_weekly_ranges  = get_map_element(rule_def, "active_weekly_ranges");
	if(active_weekly_ranges != NULL)
	{
		char* tmp = dynamic_strcat(3, " timerange weekly-ranges \\\"", active_weekly_ranges, "\\\" " );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}
	else if(active_hours != NULL && active_weekdays != NULL)
	{	
		char* tmp = dynamic_strcat(5, " timerange hours \\\"", active_hours, "\\\" weekdays \\\"", active_weekdays, "\\\" " );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}
	else if(active_hours != NULL)
	{
		char* tmp = dynamic_strcat(3, " timerange hours \\\"", active_hours, "\\\" " );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}
	else if(active_weekdays != NULL)
	{
		char* tmp = dynamic_strcat(3, " timerange weekdays \\\"", active_weekdays, "\\\" " );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}

	/*
	 * layer7 && ipp2p can not be negated.  To negate them
	 * set a mark/connmark and negate that
	 */
	// NOTE - Layer7 broken for nftables
	/*char* layer7_def = get_map_element(rule_def, "layer7");
	if(layer7_def != NULL)
	{
		char* tmp = dynamic_strcat(2, " -m layer7 --l7proto ", layer7_def );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}*/
	// NOTE - ipP2P broken for nftables
	/*char* ipp2p_def = get_map_element(rule_def, "ipp2p");
	if(ipp2p_def != NULL)
	{
		char* tmp = dynamic_strcat(2, " -m ipp2p --", ipp2p_def );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}*/
	char* min_def = get_map_element(rule_def, "min_pkt_size");
	char* max_def = get_map_element(rule_def, "max_pkt_size");
	if(min_def != NULL && max_def != NULL)
	{
		min_def = min_def == NULL ? "0" : min_def;
		max_def = max_def == NULL ? "3000" : max_def; //typical max transmission size is 1500, let's make default max 2x that
		char* tmp = dynamic_strcat(5, " meta length ", min_def, "-", max_def, " " );
		dcat_and_free(&single_check, &tmp, 1, 1);
	}

	/* make sure proto is lower case */
	char* proto = get_map_element(rule_def, "proto");
	if(proto == NULL)
	{
		proto = strdup("both");
		set_map_element(rule_def, "proto", proto);
	}
	to_lowercase(proto);
	if( safe_strcmp(proto, "udp") != 0 && safe_strcmp(proto, "tcp") != 0 && safe_strcmp(proto, "both") != 0 )
	{
		char* tmp;
		tmp = set_map_element(rule_def, "proto", strdup("both"));
		free(tmp);
		proto = (char*)get_map_element(rule_def, "proto");
	}
	int include_proto = strcmp(proto, "both") == 0 ? 0 : 1;

	/* parse multi rules */
	int mask_byte_index = 0;
	list* initial_mask_list = initialize_list();
	list* final_mask_list = initialize_list();

	/* url matches are a bit of a special case, handle them first */
	/* we have to save this mask_byte_index specially, because it must be set separately, so it only gets set if packet is http request */
	int url_mask_byte_index = mask_byte_index;

	char* url_match_vars[] = { "url_contains", "url_regex", "url_exact", "url_domain_contains", "url_domain_regex", "url_domain_exact" };
	char* url_neg_match_vars[] = { "not_url_contains", "not_url_regex", "not_url_exact", "not_url_domain_contains", "not_url_domain_regex", "not_url_domain_exact" };
	char* url_prefixes1[] = { " weburl ", " weburl ", " weburl ",  " weburl domain-only ", " weburl domain-only ", " weburl domain-only " };
	char* url_prefixes2[] = { " contains ", " contains-regex ", " matches-exactly ",  " contains ", " contains-regex ", " matches-exactly " };
	list* url_lists[6];
	int url_var_index=0;
	int url_rule_count=0;
	int url_is_negated=0;
	
	for(url_is_negated=0; url_is_negated < 2 && url_rule_count == 0; url_is_negated++)
	{
		char** url_vars = url_is_negated ? url_neg_match_vars : url_match_vars;
		for(url_var_index=0; url_var_index < 6; url_var_index++)
		{
			list* url_list = get_map_element(rule_def, url_vars[url_var_index]);
			if(url_list != NULL)
			{
				url_rule_count = url_rule_count + url_list->length;
			}
			url_lists[url_var_index] = url_list;
		}
	}
	url_is_negated--;

	proto = url_rule_count > 0 ? "tcp" : proto;
	int url_is_multi = url_rule_count <= 1 ? 0 : 1;
	for(url_var_index=0; url_var_index < 6; url_var_index++)
	{
		list* url_list = url_lists[url_var_index];
		if(url_list != NULL)
		{
			if(url_list->length > 0)
			{
				unsigned long num_vals;
				char** url_def = (char**)get_list_values(url_list, &num_vals);
				compute_multi_rules( url_def, multi_rules, &single_check, url_is_multi, rule_prefix, url_prefixes1[url_var_index], url_prefixes2[url_var_index], 0, url_is_negated, mask_byte_index, proto, include_proto, 1 );
				free(url_def);
			}
		}
	}
	push_list(initial_mask_list, (void*)&url_is_negated);
	push_list(final_mask_list, (void*)&url_is_multi);
	mask_byte_index++;

	/* mark matches */
	char** mark_def = get_map_element(rule_def, "mark");
	int mark_is_negated = mark_def == NULL ? 1 : 0;
	mark_def = mark_def == NULL ? get_map_element(rule_def, "not_mark") : mark_def;
	mark_is_negated = mark_def == NULL ? 0 : mark_is_negated;
	/* we can't do single negation with mark match, so always add seperate multi-match if mark is negated */
	int mark_is_multi = compute_multi_rules(mark_def, multi_rules, &single_check, mark_is_negated, rule_prefix, " meta mark ", "", mark_is_negated, 0, mask_byte_index, proto, include_proto, 0) == 2;
	push_list(initial_mask_list, (void*)&mark_is_negated);
	push_list(final_mask_list, (void*)&mark_is_multi);
	mask_byte_index++;	

	/* connmark matches */
	char** connmark_def = get_map_element(rule_def, "connmark");
	int connmark_is_negated = connmark_def == NULL ? 1 : 0;
	connmark_def = connmark_def == NULL ? get_map_element(rule_def, "not_connmark") : connmark_def;
	connmark_is_negated = connmark_def == NULL ? 0 : connmark_is_negated;
	int connmark_is_multi = compute_multi_rules(connmark_def, multi_rules, &single_check, 0, rule_prefix, " ct mark ", "", connmark_is_negated, 0, mask_byte_index, proto, include_proto, 0) == 2;
	push_list(initial_mask_list, (void*)&connmark_is_negated);
	push_list(final_mask_list, (void*)&connmark_is_multi);
	mask_byte_index++;	

	/*
	 * for ingress source = remote, destination = local 
	 * for egress source = local, destination = remote
	 *
	 * addresses are a bit tricky, since we need to handle 3 different kinds of matches: ips, ip6s and macs
	 */
	char*** src_def = get_map_element(rule_def, (is_ingress ? "remote_addr" : "local_addr"));
	char*** not_src_def = get_map_element(rule_def, (is_ingress ? "not_remote_addr" : "not_local_addr"));
	int src_is_negated = src_def == NULL  && not_src_def != NULL ? 1 : 0;
	src_def = src_is_negated == 1 ? not_src_def : src_def;

	char*** dst_def = get_map_element(rule_def, (is_ingress ? "local_addr"  : "remote_addr"));
	char*** not_dst_def = get_map_element(rule_def, (is_ingress ? "not_local_addr"  : "not_remote_addr"));
	int dst_is_negated = dst_def == NULL && not_dst_def != NULL ? 1 : 0;
	dst_def = dst_is_negated == 1 ? not_dst_def : dst_def;

	char*** addr_defs[2] = { src_def, dst_def };
	int addr_negated[2] = { src_is_negated, dst_is_negated };
	char* addr_prefix1[2][3] = { { " ip saddr ", " ip6 saddr ", " ether saddr " }, { " ip daddr", " ip6 daddr ", NULL } };

	int addr_index = 0;
	int is_true = 1;
	int is_false = 0;
	for(addr_index = 0; addr_index < 2; addr_index++)
	{
		char*** addrs = addr_defs[addr_index];
		if(addrs != NULL)
		{
			int total_rules = 0;
			int test_list_index;
			for(test_list_index=0; test_list_index < 3; test_list_index++)
			{
				char** test_list = addrs[test_list_index];
				if(test_list != NULL && addr_prefix1[addr_index][test_list_index] != NULL)
				{
					while(test_list[0] != NULL){ test_list++; total_rules++; }
				}
			}
			int is_multi = total_rules > 1 ? 1 : 0;
			int is_negated = addr_negated[addr_index];
			//printf("is negated = %d for addr_index = %d\n", is_negated, addr_index);

			for(test_list_index=0; test_list_index < 3; test_list_index++)
			{
				char** test_list = addrs[test_list_index];
				if(test_list != NULL && addr_prefix1[addr_index][test_list_index] != NULL)
				{
					if(test_list[0] != NULL)
					{
						compute_multi_rules(test_list, multi_rules, &single_check, is_multi, rule_prefix, addr_prefix1[addr_index][test_list_index], "",is_negated, 0, mask_byte_index, proto, include_proto, 0);
					}
				}
			}
			
			push_list(initial_mask_list, (void*)(addr_negated + addr_index));
			push_list(final_mask_list, (void*)(is_multi == 1 ? &is_true : &is_false) );
			mask_byte_index++;
		}
	}

	char** sport_def = get_map_element(rule_def, (is_ingress ? "remote_port" : "local_port"));
	int sport_is_negated = sport_def == NULL ? 1 : 0;
	sport_def = sport_def == NULL ? get_map_element(rule_def, (is_ingress ? "not_remote_port" : "not_local_port")) : sport_def;
	sport_is_negated = sport_def == NULL ? 0 : sport_is_negated;
	int sport_is_multi = compute_multi_rules(sport_def, multi_rules, &single_check, 0, rule_prefix, " sport ", "", sport_is_negated, 0, mask_byte_index, proto, 1, 0) == 2;
	push_list(initial_mask_list, (void*)&sport_is_negated);
	push_list(final_mask_list, (void*)&sport_is_multi);
	mask_byte_index++;

	char** dport_def = get_map_element(rule_def, (is_ingress ? "local_port"  : "remote_port"));
	int dport_is_negated = dport_def == NULL ? 1 : 0;
	dport_def = dport_def == NULL ? get_map_element(rule_def, (is_ingress ? "not_local_port" : "not_remote_port")) : dport_def;
	dport_is_negated = dport_def == NULL ? 0 : dport_is_negated;
	int dport_is_multi = compute_multi_rules(dport_def, multi_rules, &single_check, 0, rule_prefix, " dport ", "", dport_is_negated, 0, mask_byte_index, proto, 1, 0) == 2;
	push_list(initial_mask_list, (void*)&dport_is_negated);
	push_list(final_mask_list, (void*)&dport_is_multi);
	mask_byte_index++;

	list* all_rules = initialize_list();

	//if no target_options specified, make sure it's an empty string, not null
	target_options = (target_options == NULL) ? "" : target_options;
	//if target_options is empty and we're rejecting and proto is tcp, set options to --reject-with tcp-reset instead of default
	target_options = strlen(target_options) == 0 && (strcmp(proto, "tcp") == 0) && strcmp(target, "reject") == 0 ? " with tcp reset " : target_options;
	if(multi_rules->length > 0)
	{
		if(strlen(single_check) > 0)
		{
			char* dummy_multi[] = { single_check, NULL };
			int requires_proto = strcmp(proto, "both") == 0 && sport_def == NULL && dport_def == NULL ? 0 : 1;
			compute_multi_rules(dummy_multi, multi_rules, &single_check, 1, rule_prefix, " ", "", 0, 0, mask_byte_index, proto, requires_proto, 0);
			mask_byte_index++;
		}

		/*
		printf("final mask length = %ld\n", final_mask_list->length);
		printf("src is multi = %d\n", src_is_multi);
		unsigned long mi;
		int* one = shift_list(final_mask_list);
		int* two = shift_list(final_mask_list);
		printf("one = %d, two = %d\n", *one, *two);
		unshift_list(final_mask_list, two);
		unshift_list(final_mask_list, one);
		*/

		unsigned long initial_url_mark = 0x01000000 * url_is_negated * url_is_multi;
		unsigned long initial_main_mark = 0;
		unsigned long final_match = 0;
		int next_mask_index;
		for(next_mask_index = 0; next_mask_index <mask_byte_index ; next_mask_index++)
		{
			int tmp  = 1;
			int* next_is_multi = &tmp;
			if(final_mask_list->length > 0)
			{
				next_is_multi = shift_list(final_mask_list);
			}
			else
			{
				*next_is_multi = 1;
			}

			unsigned long next_mark_bit = 0x01000000 * (unsigned long)pow(2, next_mask_index) * (*next_is_multi);
			final_match = final_match + next_mark_bit;
			if(initial_mask_list->length > 0)
			{
				int* is_negation = (int*)shift_list(initial_mask_list);
				if(*is_negation == 1 && next_mask_index != url_mask_byte_index )
				{
					//printf("nonzero byte index for main mark = %d\n", next_mask_index);
					initial_main_mark = initial_main_mark + next_mark_bit;
				}
			}
			/* else it's last single_check mark which is never initialized to one */
		}

		if(initial_main_mark > 0)
		{
			//set main_mark unconditionally
			char mark[12];
			sprintf(mark, "0x%lX", initial_main_mark);
			push_list(all_rules, dynamic_strcat(3, rule_prefix, " ct mark set ct mark \\& 0x00FFFFFF \\| ", mark ));
		}
		if(initial_url_mark > 0) //do url_mark second since because in order to set main mark we use full mask of 0xFF000000
		{
			//set proper mark if this is an http request
			char mark[12];
			char* inverted_mark = NULL;
			sprintf(mark, "0x%lX", initial_url_mark);
			inverted_mark = invert_bitmask(mark);
			push_list(all_rules, dynamic_strcat(5,  rule_prefix, " meta l4proto tcp weburl contains \\\"http\\\" ct mark set ct mark \\& ", inverted_mark, " \\| ", mark));
			free(inverted_mark);
		}
		
		//put all rules in place from multi_rules list
		while(multi_rules->length > 0)
		{
			push_list(all_rules, shift_list(multi_rules));
		}
		unsigned long tmp_length;
		destroy_list(multi_rules, DESTROY_MODE_IGNORE_VALUES, &tmp_length);

		//if final mark matches perfectly with mask of 0xFF000000, jump to  (REJECT/ACCEPT) target
		char final_match_str[12];
		sprintf(final_match_str, "0x%lX", final_match);
		
		//if we're rejecting, no target options are specified, and no proto is specified add two rules: one for tcp with tcp-reject, and one for everything else
		if(safe_strcmp(target, "reject") == 0 && safe_strcmp(target_options, "") == 0 && safe_strcmp(proto, "both"))
		{
			push_list(all_rules, dynamic_strcat(4, rule_prefix, " meta l4proto tcp ct mark \\& 0xFF000000 == ", final_match_str, " reject with tcp reset"));
			push_list(all_rules, dynamic_strcat(4, rule_prefix, " ct mark \\& 0xFF000000 == ", final_match_str, " reject"));

		}
		else
		{
			char* final_proto = strstr(target_options, "tcp reset") == NULL ? "" : " meta l4proto tcp ";
			push_list(all_rules, dynamic_strcat(7, rule_prefix, final_proto, " ct mark \\& 0xFF000000 == ", final_match_str, " ", target, target_options ));
		}
		//if final mark does not match (i.e. we didn't reject), unconditionally reset mark to 0x0 with mask of 0xFF000000
		push_list(all_rules, dynamic_strcat(2, rule_prefix,  " ct mark set ct mark \\& 0x00FFFFFF \\| 0x0" ));
	}
	else
	{
		if( strcmp(proto, "both") == 0 )
		{	
			if( dport_def == NULL && sport_def == NULL )
			{
				if(safe_strcmp(target, "reject") == 0 && safe_strcmp(target_options, "") == 0 )
				{
					push_list(all_rules, dynamic_strcat(4, rule_prefix, " meta l4proto tcp ", single_check, " reject with tcp reset"));
				}
				push_list(all_rules, dynamic_strcat(5, rule_prefix, single_check, " ",target, target_options ));
			}
			else
			{
				if(safe_strcmp(target, "reject") == 0 && safe_strcmp(target_options, "") == 0 )
				{
					push_list(all_rules, dynamic_strcat(4, rule_prefix, " meta l4proto tcp ", single_check, " reject with tcp reset" ));
				}
				else
				{
					push_list(all_rules, dynamic_strcat(6, rule_prefix, " meta l4proto tcp ", single_check, " ", target, target_options ));
				}
				push_list(all_rules, dynamic_strcat(6, rule_prefix, " meta l4proto udp ", single_check, " ", target, target_options ));
			}
		}
		else
		{
			push_list(all_rules, dynamic_strcat(8, rule_prefix, " meta l4proto ", proto, " ", single_check, " ", target, target_options ));
		}
	}

	/* handle very special case: if we're white-listing a URL we need to make
	 * sure other, non-request packets in connection get through too.  So, we allow all traffic
	 * with a destination port of 80 that is NOT an HTTP request.  This should allow
	 * HTTP connections to persist but prevent connections to any websites but those
	 * specified
	 */
	if(url_rule_count > 0 && safe_strcmp(target, "accept") == 0 && is_ingress == 0)
	{
		push_list(all_rules, dynamic_strcat(2, rule_prefix, " meta l4proto tcp weburl contains \\\"http\\\" ct mark set ct mark \\& 0x00FFFFFF \\| 0xFF000000" ));
		push_list(all_rules, dynamic_strcat(2, rule_prefix, " meta l4proto tcp dport {80,443} ct mark \\& 0xFF000000 != 0xFF000000 accept " ));
		push_list(all_rules, dynamic_strcat(2, rule_prefix,  " meta l4proto tcp ct mark \\& 0xFF000000 == 0xFF000000 reject with tcp reset" ));
		push_list(all_rules, dynamic_strcat(2, rule_prefix,  " ct mark set ct mark \\& 0x00FFFFFF \\| 0x0" ));
	}

	unsigned long num_rules;
	char** block_rule_list = (char**) destroy_list( all_rules, DESTROY_MODE_RETURN_VALUES, &num_rules);

	return block_rule_list;
}

/* returns 0 if no rules found, 1 if one rule found AND included in single_check, otherwise 2 */
int compute_multi_rules(char** def, list* multi_rules, char** single_check, int never_single, char* rule_prefix, char* test_prefix1, char* test_prefix2, int is_negation1, int is_negation2, int mask_byte_index, char* proto, int requires_proto, int quoted_args)
{
	int parse_type = 0;
	int is_negation = 0;
	if(is_negation1 == 1 || is_negation2 == 1)
	{
		is_negation = 1;
	}
	if(def != NULL)
	{
		int num_rules; 
		for(num_rules=0; def[num_rules] != NULL; num_rules++){}
		if(num_rules == 1 && !never_single)
		{
			parse_type = 1;
			// URL: weburl ! contains \"blah blah\"
			char* tmp = dynamic_strcat(7, test_prefix1, (is_negation1 ? " != " : " "), test_prefix2, (is_negation2 ? " != " : " "), (quoted_args ? " \\\"" : " "), def[0], (quoted_args ? "\\\" " : " ") );
			dcat_and_free(&tmp, single_check, 1, 1 );
		}
		else
		{
			parse_type = 2;
			unsigned long mask = 0x01000000 * (unsigned long)pow(2, mask_byte_index);
			char mask_str[12];
			char* inverted_mask = NULL;
			sprintf(mask_str, "0x%lX", mask);
			inverted_mask = invert_bitmask(mask_str);
			char* connmark_part = dynamic_strcat(4, " ct mark set ct mark \\& ", inverted_mask, " \\| ", (is_negation ? "0x0" : mask_str));
			free(inverted_mask);

			int rule_index =0;
			for(rule_index=0; def[rule_index] != NULL; rule_index++)
			{
				char* common_part = dynamic_strcat(7, test_prefix1, " ", test_prefix2, (quoted_args ? " \\\"" : " "),  def[rule_index], (quoted_args ? "\\\" " : " "), connmark_part);
				if(strcmp(proto, "both") == 0)
				{
					if(requires_proto)
					{
						push_list(multi_rules, dynamic_strcat(3, rule_prefix, " meta l4proto {tcp,udp} ", common_part));
					}
					else
					{
						push_list(multi_rules, dynamic_strcat(3, rule_prefix, " ", common_part ));
					}
				}
				else
				{
					push_list(multi_rules, dynamic_strcat(5, rule_prefix, " meta l4proto ", proto, " ", common_part));
				}
				free(common_part);
			}
			free(connmark_part);
		}
	}
	return parse_type;
}

string_map* get_rule_definition(char* package, char* section)
{
	string_map* definition = NULL;
	struct uci_context *ctx;
	struct uci_package *p = NULL;
	ctx = uci_alloc_context();
	if(uci_load(ctx, package, &p) == UCI_OK)
	{
		struct uci_ptr ptr;
		char* lookup_str = dynamic_strcat(3, package, ".", section);
		int ret_value = uci_lookup_ptr(ctx, &ptr, lookup_str, 1);
		if(ret_value == UCI_OK)
		{
			struct uci_section *s = ptr.s;
			if(s != NULL)
			{
				struct uci_element *e;
				definition = initialize_string_map(1);
				
				uci_foreach_element(&s->options, e) 
				{
					char* option_name = strdup(e->name);
					to_lowercase(option_name);
					char* option_value = get_option_value_string(uci_to_option(e));
					parse_option(option_name, option_value, definition);
					free(option_name);
					free(option_value);
				}
			}
		}
	}
	uci_free_context(ctx);
	
	return definition;
}

int parse_option(char* option_name, char* option_value, string_map* definition)
{
	int valid_option = 0;
	if(	safe_strcmp(option_name, "proto") == 0 ||
		safe_strcmp(option_name, "layer7") == 0 ||
		safe_strcmp(option_name, "ipp2p") == 0 ||
		safe_strcmp(option_name, "max_pkt_size") == 0 || 
		safe_strcmp(option_name, "min_pkt_size") ==0 ||
		safe_strcmp(option_name, "family") ==0
		)
	{
		valid_option = 1;
		set_map_element(definition, option_name, strdup(option_value));
	}
	else if(	safe_strcmp(option_name, "active_hours") == 0 || 
			safe_strcmp(option_name, "active_weekly_ranges") == 0 || 
			safe_strcmp(option_name, "active_weekdays") == 0
			)
	{
		to_lowercase(option_value);
		if( safe_strcmp(option_value, "all") != 0 )
		{
			valid_option = 1;
			set_map_element(definition, option_name, strdup(option_value));
		}
	}
	else if(	safe_strcmp(option_name, "mark") == 0 ||
			safe_strcmp(option_name, "connmark") == 0 ||
			safe_strcmp(option_name, "not_mark") == 0 ||
			safe_strcmp(option_name, "not_connmark") == 0
			)
	{
		valid_option = 1;
		set_map_element(definition, option_name, parse_marks(option_value, 0xFFFFFFFF));
	}
	else if(	safe_strcmp(option_name, "remote_addr") == 0 ||
			safe_strcmp(option_name, "local_addr") == 0 ||
			safe_strcmp(option_name, "not_remote_addr") == 0 ||
			safe_strcmp(option_name, "not_local_addr") == 0 
		       		)
	{
		char*** parsed_addr = parse_ips_and_macs(option_value);
		if(parsed_addr != NULL)
		{
			valid_option = 1;
			if( safe_strcmp(option_name, "not_remote_addr") == 0  || safe_strcmp(option_name, "remote_addr") == 0 )
			{
				parsed_addr[MATCH_MAC_INDEX][0] = NULL; //doesn't make sense to match remote MAC address
			}
			set_map_element(definition, option_name, parsed_addr);
		}
	}
	else if(	safe_strcmp(option_name, "remote_port") == 0 ||
			safe_strcmp(option_name, "local_port") == 0 ||
			safe_strcmp(option_name, "not_remote_port") == 0 ||
			safe_strcmp(option_name, "not_local_port") == 0 
			)
	{
		char** parsed_ports = parse_ports(option_value);
		if(parsed_ports != NULL)
		{
			valid_option = 1;
			set_map_element(definition, option_name, parsed_ports);
		}
	}
	else if(	truncate_if_starts_with(option_name, "url_contains") ||
			truncate_if_starts_with(option_name, "url_regex") ||
			truncate_if_starts_with(option_name, "url_exact") ||
			truncate_if_starts_with(option_name, "url_domain_contains") ||
			truncate_if_starts_with(option_name, "url_domain_regex") ||
			truncate_if_starts_with(option_name, "url_domain_exact")  ||
			truncate_if_starts_with(option_name, "not_url_contains")  ||
			truncate_if_starts_with(option_name, "not_url_regex")  ||
			truncate_if_starts_with(option_name, "not_url_exact")  ||
			truncate_if_starts_with(option_name, "not_url_domain_contains") ||
			truncate_if_starts_with(option_name, "not_url_domain_regex")  ||
			truncate_if_starts_with(option_name, "not_url_domain_exact")  
			)
	{
		/*
		 * may be a quoted list of urls to block, so attempt to parse this
		 * if no quotes found, match on unquoted expresssion
		 * we don't need to de-escape quotes because when we define rule, 
		 * we call nftables from system, and through the shell, which will de-escape quotes for us
		 */
		list* parsed_quoted = parse_quoted_list(option_value, '\"', '\\', 0);
		list* old_parsed = get_map_element(definition, option_name);
		if(old_parsed != NULL)
		{
			while(parsed_quoted->length > 0)
			{
				push_list(old_parsed, shift_list(parsed_quoted));
			}
			free(parsed_quoted);
			parsed_quoted = old_parsed;
		}
		if(parsed_quoted->length > 0)
		{
			valid_option = 1;
			set_map_element(definition, option_name, parsed_quoted);
		}
	}
	return valid_option;
}

// this function dynamically allocates memory for
// the option string, but since this program exits
// almost immediately (after printing variable info)
// the massive memory leak we're opening up shouldn't
// cause any problems.  This is your reminder/warning
// that this might be an issue if you use this code to
// do anything fancy.
char* get_option_value_string(struct uci_option* uopt)
{
	char* opt_str = NULL;
	if(uopt->type == UCI_TYPE_STRING)
	{
		opt_str = strdup(uopt->v.string);
	}
	if(uopt->type == UCI_TYPE_LIST)
	{
		struct uci_element* e;
		uci_foreach_element(&uopt->v.list, e)
		{
			if(opt_str == NULL)
			{
				opt_str = strdup(e->name);
			}
			else
			{
				char* tmp;
				tmp = dynamic_strcat(3, opt_str, " ", e->name);
				free(opt_str);
				opt_str = tmp;
			}
		}
	}

	return opt_str;
}

char*** parse_ips_and_macs(char* addr_str)
{
	unsigned long num_pieces;
	char** addr_parts = split_on_separators(addr_str, ",", 1, -1, 0, &num_pieces);
	list* ip_list = initialize_list();
	list* ip6_list = initialize_list();
	list* mac_list = initialize_list();
	
	int ip_part_index;
	for(ip_part_index=0; addr_parts[ip_part_index] != NULL; ip_part_index++)
	{
		char* next_str = addr_parts[ip_part_index];
		//ipv4, ipv6 or any can be MAC address. Test it first
		if(strchr(next_str, ':'))
		{
			trim_flanking_whitespace(next_str);
			int macaddr[6];
			if(sscanf(next_str, "%2x:%2x:%2x:%2x:%2x:%2x", macaddr, macaddr+1, macaddr+2, macaddr+3, macaddr+4, macaddr+5) == 6)
			{
				push_list(mac_list, trim_flanking_whitespace(next_str));
			}
		}

		if(strchr(next_str, '-') != NULL)
		{
			char** range_parts = split_on_separators(next_str, "-", 1, 2, 1, &num_pieces);
			char* start = trim_flanking_whitespace(range_parts[0]);
			char* end = trim_flanking_whitespace(range_parts[1]);
			
			char start_ip[16];
			char end_ip[16];
			if(inet_pton(AF_INET6, start, start_ip) && inet_pton(AF_INET6, end, end_ip))
			{
				push_list(ip6_list, trim_flanking_whitespace(next_str));
			}
			else if(inet_pton(AF_INET, start, start_ip) && inet_pton(AF_INET, end, end_ip))
			{
				push_list(ip_list, trim_flanking_whitespace(next_str));
			}

			free(start);
			free(end);
			free(range_parts);
		}
		else
		{
			char parsed_ip[16];
			if(inet_pton(AF_INET6, next_str, parsed_ip) == 1)
			{
				push_list(ip6_list, trim_flanking_whitespace(next_str));
			}
			else if(inet_pton(AF_INET, next_str, parsed_ip) == 1)
			{
				push_list(ip_list, trim_flanking_whitespace(next_str));
			}
		}
		
	}
	free(addr_parts);

	char*** return_value = (char***)malloc(3*sizeof(char**));

	unsigned long num1, num2, num3;
	char** ip_strs = (char**)destroy_list(ip_list, DESTROY_MODE_RETURN_VALUES, &num1);
	char** ip6_strs = (char**)destroy_list(ip6_list, DESTROY_MODE_RETURN_VALUES, &num2);
	char** mac_strs = (char**)destroy_list(mac_list, DESTROY_MODE_RETURN_VALUES, &num3);

	char* match_ip_str = join_strs(",", ip_strs, -1, 1, 1);
	char* match_ip6_str = join_strs(",", ip6_strs, -1, 1, 1);
	char* match_mac_str = join_strs(",", mac_strs, -1, 1, 1);

	if(num1 > 1)
	{
		char* tmp = dynamic_strcat(3, "{", match_ip_str, "}");
		free(match_ip_str);
		match_ip_str = tmp;
	}
	if(num2 > 1)
	{
		char* tmp = dynamic_strcat(3, "{", match_ip6_str, "}");
		free(match_ip6_str);
		match_ip6_str = tmp;
	}
	if(num3 > 1)
	{
		char* tmp = dynamic_strcat(3, "{", match_mac_str, "}");
		free(match_mac_str);
		match_mac_str = tmp;
	}
	
	if(num1 + num2 + num3 == 0)
	{
		free(return_value);
		return_value = NULL;
	}
	else
	{
		return_value[MATCH_IP_INDEX] = (char**)malloc(2*sizeof(char*));
		return_value[MATCH_IP6_INDEX] = (char**)malloc(2*sizeof(char*));
		return_value[MATCH_MAC_INDEX] = (char**)malloc(2*sizeof(char*));

		return_value[MATCH_IP_INDEX][0] = match_ip_str;
		return_value[MATCH_IP_INDEX][1] = NULL;
		return_value[MATCH_IP6_INDEX][0] = match_ip6_str;
		return_value[MATCH_IP6_INDEX][1] = NULL;
		return_value[MATCH_MAC_INDEX][0] = match_mac_str;
		return_value[MATCH_MAC_INDEX][1] = NULL;
	}

	return return_value;
}

char** parse_ports(char* port_str)
{
	unsigned long num_pieces;
	char** ports = split_on_separators(port_str, ",", 1, -1, 0, &num_pieces);
	int port_index = 0;
	for(port_index=0; ports[port_index] != NULL; port_index++)
	{
		char* dash_ptr;
		while((dash_ptr=strchr(ports[port_index], ':')) != NULL)
		{
			dash_ptr[0] = '-';
		}
		trim_flanking_whitespace( ports[port_index] );
	}
	return ports;
}

/*
 * parses a list of marks/connmarks
 * the max_mask parameter specfies a maximal mask that will be used
 * when matching marks/connmarks.  If a user-defined mask is specified
 * (by defining [mark]/[mask]) this is bitwise-anded with the maximum
 * mask to get the final mask.  This is especially necessary for
 * connmarks, because the mechanism to handle negation when multiple
 * test rules are needed uses the last (high) byte of the connmark 
 * address space, so this HAS to be masked out when matching 
 * connmarks, using max_mask=0x00FFFFFF
 */
char** parse_marks(char* list_str, unsigned long max_mask)
{
	char** marks = NULL;
	if(list_str != NULL)
	{
		unsigned long num_pieces;
		marks = split_on_separators(list_str, ",", 1, -1, 0, &num_pieces);
		if(marks[0] == NULL)
		{
			free(marks);
			marks = NULL;
		}
		else 
		{
			int mark_index;
			for(mark_index = 0; marks[mark_index] != NULL; mark_index++)
			{
				trim_flanking_whitespace(marks[mark_index]);
				if(max_mask != 0xFFFFFFFF)
				{

					char* m = marks[mark_index];
					char* mask_start;
					if( (mask_start = strchr(m, '/')) != NULL )
					{
						unsigned long mask = 0xFFFFFFFF;
						mask_start++;
						sscanf(mask_start, "%lX", &mask);
					
						mask = mask & max_mask;
					
						*(mask_start) = '\0';
						char new_mask_str[12];
						sprintf(new_mask_str, "0x%lX", mask);
						marks[mark_index] = dynamic_strcat(2, m, new_mask_str);
					}
					else
					{
						char new_mask_str[12];
						sprintf(new_mask_str, "0x%lX", max_mask);
						marks[mark_index] = dynamic_strcat(3, m, "/", new_mask_str);
					}
					free(m);
				}
			}
		}
	}
	return marks;
}

/* 
 * parses list of quoted strings, ignoring escaped quote characters that are not themselves escaped 
 * Note that we don't de-escape anything here.  If necessary that should be done elsewhere.
 */ 
list* parse_quoted_list(char* list_str, char quote_char, char escape_char, char add_remainder_if_uneven_quotes)
{
	
	long num_quotes = 0;
	long list_index = 0;
	char previous_is_quoted = 0;
	for(list_index=0; list_str[list_index] != '\0'; list_index++)
	{
		num_quotes = num_quotes + ( list_str[list_index] == quote_char && !previous_is_quoted ? 1 : 0);
		previous_is_quoted = list_str[list_index] == escape_char && !previous_is_quoted ? 1 : 0;
	}
	
	char** pieces = (char**)malloc( ((long)(num_quotes/2)+2) * sizeof(char*) );
	long piece_index = 0;
	long next_start_index=-1;
	previous_is_quoted = 0;	
	for(list_index=0; list_str[list_index] != '\0'; list_index++)
	{
		if( list_str[list_index] == quote_char && !previous_is_quoted )
		{
			if(next_start_index < 0)
			{
				next_start_index = list_index+1;
			}
			else
			{
				long length = list_index-next_start_index;
				char* next_piece = (char*)malloc( (1+length)*sizeof(char) );
				memcpy(next_piece, list_str+next_start_index, length);
				next_piece[length] = '\0';
				pieces[piece_index] = next_piece;
				piece_index++;
				next_start_index = -1;
			}
		}
		previous_is_quoted = list_str[list_index] == escape_char && !previous_is_quoted ? 1 : 0;
	}
	if(add_remainder_if_uneven_quotes && next_start_index >= 0)
	{
		long length = 1+list_index-next_start_index;
		char* next_piece = (char*)malloc( (1+length)*sizeof(char) );
		memcpy(next_piece, list_str+next_start_index, length);
		next_piece[length] = '\0';
		pieces[piece_index] = next_piece;
		piece_index++;
	}
	pieces[piece_index] = NULL;

	list* quoted_list = initialize_list();
	if(pieces[0] != NULL)
	{
		for(piece_index=0; pieces[piece_index] != NULL; piece_index++)
		{
			push_list(quoted_list, pieces[piece_index]);
			//don't free pieces[piece], we're just putting it in list
		}
	}
	else
	{
		//if no quotes at all, just return list_str
		push_list(quoted_list, strdup(list_str));
	}
	free(pieces);//but do free array of char* pointers, we don't need it anymore
	
	return quoted_list;
}

int truncate_if_starts_with(char* test_str, char* prefix)
{
	int prefix_length = strlen(prefix);
	int test_length = strlen(test_str);
	int matches = 0;
	if(prefix_length <= test_length)
	{
		matches = strncmp(test_str, prefix, prefix_length) == 0 ? 1 : 0;
		if(matches)
		{
			test_str[prefix_length] = '\0';
		}
	}
	return matches;
}

// Accepts bitmask as either hex or decimal string (e.g. 0xFF00 or 65280) and returns the inverted bitmask in hex (0x00FF)
char* invert_bitmask(const char* input)
{
    if (!input || *input == '\0')
	{
        return NULL;
    }

    uint32_t value = 0;
    int num_bits = 0;
    int is_hex = 0;

    // Check if input starts with "0x" or "0X"
    if (strncmp(input, "0x", 2) == 0 || strncmp(input, "0X", 2) == 0)
	{
        is_hex = 1;
        const char* hex = input + 2;
        size_t len = strlen(hex);
        if (len > 8) return NULL; // max 32 bits (8 hex digits)

        while (*hex)
		{
            char c = *hex++;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (c - '0');
            else if (c >= 'a' && c <= 'f') value |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (c - 'A' + 10);
            else return NULL;
        }

        num_bits = len * 4; // each hex digit is 4 bits
    }
	else
	{
        // Parse as decimal
        char* endptr = NULL;
        unsigned long parsed = strtoul(input, &endptr, 10);
        if (*endptr != '\0' || parsed > 0xFFFFFFFFUL)
		{
            return NULL; // invalid or too large for 32-bit
        }
        value = (uint32_t)parsed;

        // Determine how many bits are needed to represent the value
        for (int i = 31; i >= 0; i--)
		{
            if (value & (1U << i))
			{
                num_bits = i + 1;
                break;
            }
        }
        if (num_bits == 0) num_bits = 1; // handle value == 0
    }

    uint32_t mask = (num_bits == 32) ? 0xFFFFFFFF : ((1U << num_bits) - 1);
    uint32_t inverted = ~value & mask;

    // Allocate result string: "0x" + 8 hex digits + null terminator
    char* result = malloc(2 + 8 + 1);
    if (!result) return NULL;

    sprintf(result, "0x%0*X", (num_bits + 3) / 4, inverted); // pad to full hex digits
    return result;
}
