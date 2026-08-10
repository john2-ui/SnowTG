/**
 * @file dns_scenario.c
 * @brief Compiles the DNS object in a traffic-generator scenario.
 */

#include "dns_scenario.h"

#include "../../core/scenario.h"
#include "dns_client.h"

#include <errno.h>
#include <stdlib.h>

/** @copydoc tg_dns_scenario_compile */
int tg_dns_scenario_compile(const struct tg_json_doc *doc, int object_index,
                            struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"qname", "qtype"};
        struct tg_dns_config *config;
        int qname_index;
        int qtype_index;

        if (doc == NULL || doc->tokens == NULL || class_plan == NULL ||
            object_index < 0 || object_index >= doc->token_count ||
            doc->tokens[object_index].type != JSMN_OBJECT ||
            !tg_json_object_has_only(doc, object_index, allowed,
                                     sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        config = calloc(1, sizeof(*config));
        if (config == NULL)
                return -1;

        qname_index = tg_json_object_value(doc, object_index, "qname");
        qtype_index = tg_json_object_value(doc, object_index, "qtype");
        if (qname_index < 0 || qtype_index < 0 ||
            tg_json_copy_string(config->qname, sizeof(config->qname), doc,
                                &doc->tokens[qname_index]) != 0 ||
            tg_dns_qname_validate(config->qname) != 0) {
                free(config);
                errno = EINVAL;
                return -1;
        }
        if (tg_json_token_equal(doc, &doc->tokens[qtype_index], "A"))
                config->qtype = TG_DNS_QTYPE_A;
        else if (tg_json_token_equal(doc, &doc->tokens[qtype_index], "AAAA"))
                config->qtype = TG_DNS_QTYPE_AAAA;
        else {
                free(config);
                errno = EINVAL;
                return -1;
        }
        config->transaction_id = TG_DNS_TRANSACTION_ID;
        class_plan->proto = &tg_dns_proto_ops;
        class_plan->proto_config = config;
        if (class_plan->proto->build_request(
                config, class_plan->request_template,
                sizeof(class_plan->request_template),
                &class_plan->request_template_len) != 0 ||
            class_plan->request_template_len == 0) {
                int saved_errno = errno == 0 ? EINVAL : errno;

                class_plan->proto_config = NULL;
                free(config);
                errno = saved_errno;
                return -1;
        }
        return 0;
}
