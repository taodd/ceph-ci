// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <map>
#include <string>

#include "common/armor.h"
#include "common/utf8.h"
#include "rgw_common.h"
#include "rgw_client_io.h"
#include "rgw_rest.h"
#include "rgw_crypt_sanitize.h"
#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

static const auto signed_subresources = {
  "acl",
  "cors",
  "delete",
  "lifecycle",
  "location",
  "logging",
  "notification",
  "partNumber",
  "policy",
  "requestPayment",
  "response-cache-control",
  "response-content-disposition",
  "response-content-encoding",
  "response-content-language",
  "response-content-type",
  "response-expires",
  "torrent",
  "uploadId",
  "uploads",
  "start-date",
  "end-date",
  "versionId",
  "versioning",
  "versions",
  "website"
};

/*
 * ?get the canonical amazon-style header for something?
 */

static std::string
get_canon_amz_hdr(const std::map<std::string, std::string>& meta_map)
{
  std::string dest;

  for (const auto& kv : meta_map) {
    dest.append(kv.first);
    dest.append(":");
    dest.append(kv.second);
    dest.append("\n");
  }

  return dest;
}

/*
 * ?get the canonical representation of the object's location
 */
static std::string
get_canon_resource(const char* const request_uri,
                   const std::map<std::string, std::string>& sub_resources)
{
  std::string dest;

  if (request_uri) {
    dest.append(request_uri);
  }

  bool initial = true;
  for (const auto& subresource : signed_subresources) {
    const auto iter = sub_resources.find(subresource);
    if (iter == std::end(sub_resources)) {
      continue;
    }
    
    if (initial) {
      dest.append("?");
      initial = false;
    } else {
      dest.append("&");
    }

    dest.append(iter->first);
    if (! iter->second.empty()) {
      dest.append("=");
      dest.append(iter->second);
    }
  }

  dout(10) << "get_canon_resource(): dest=" << dest << dendl;
  return dest;
}

/*
 * get the header authentication  information required to
 * compute a request's signature
 */
void rgw_create_s3_canonical_header(
  const char* const method,
  const char* const content_md5,
  const char* const content_type,
  const char* const date,
  const std::map<std::string, std::string>& meta_map,
  const char* const request_uri,
  const std::map<std::string, std::string>& sub_resources,
  std::string& dest_str)
{
  std::string dest;

  if (method) {
    dest = method;
  }
  dest.append("\n");
  
  if (content_md5) {
    dest.append(content_md5);
  }
  dest.append("\n");

  if (content_type) {
    dest.append(content_type);
  }
  dest.append("\n");

  if (date) {
    dest.append(date);
  }
  dest.append("\n");

  dest.append(get_canon_amz_hdr(meta_map));
  dest.append(get_canon_resource(request_uri, sub_resources));

  dest_str = dest;
}

int rgw_get_s3_header_digest(const string& auth_hdr, const string& key, string& dest)
{
  if (key.empty())
    return -EINVAL;

  char hmac_sha1[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];
  calc_hmac_sha1(key.c_str(), key.size(), auth_hdr.c_str(), auth_hdr.size(), hmac_sha1);

  char b64[64]; /* 64 is really enough */
  int ret = ceph_armor(b64, b64 + 64, hmac_sha1,
		       hmac_sha1 + CEPH_CRYPTO_HMACSHA1_DIGESTSIZE);
  if (ret < 0) {
    dout(10) << "ceph_armor failed" << dendl;
    return ret;
  }
  b64[ret] = '\0';

  dest = b64;

  return 0;
}

static inline bool is_base64_for_content_md5(unsigned char c) {
  return (isalnum(c) || isspace(c) || (c == '+') || (c == '/') || (c == '='));
}

/*
 * get the header authentication  information required to
 * compute a request's signature
 */
bool rgw_create_s3_canonical_header(const req_info& info,
                                    utime_t* const header_time,
                                    std::string& dest,
                                    const bool qsr)
{
  const char* const content_md5 = info.env->get("HTTP_CONTENT_MD5");
  if (content_md5) {
    for (const char *p = content_md5; *p; p++) {
      if (!is_base64_for_content_md5(*p)) {
        dout(0) << "NOTICE: bad content-md5 provided (not base64),"
                << " aborting request p=" << *p << " " << (int)*p << dendl;
        return false;
      }
    }
  }

  const char *content_type = info.env->get("CONTENT_TYPE");

  std::string date;
  if (qsr) {
    date = info.args.get("Expires");
  } else {
    const char *str = info.env->get("HTTP_DATE");
    const char *req_date = str;
    if (str) {
      date = str;
    } else {
      req_date = info.env->get("HTTP_X_AMZ_DATE");
      if (!req_date) {
        dout(0) << "NOTICE: missing date for auth header" << dendl;
        return false;
      }
    }

    if (header_time) {
      struct tm t;
      if (!parse_rfc2616(req_date, &t)) {
        dout(0) << "NOTICE: failed to parse date for auth header" << dendl;
        return false;
      }
      if (t.tm_year < 70) {
        dout(0) << "NOTICE: bad date (predates epoch): " << req_date << dendl;
        return false;
      }
      *header_time = utime_t(internal_timegm(&t), 0);
    }
  }

  const auto& meta_map = info.x_meta_map;
  const auto& sub_resources = info.args.get_sub_resources();

  std::string request_uri;
  if (info.effective_uri.empty()) {
    request_uri = info.request_uri;
  } else {
    request_uri = info.effective_uri;
  }

  rgw_create_s3_canonical_header(info.method, content_md5, content_type,
                                 date.c_str(), meta_map, request_uri.c_str(),
                                 sub_resources, dest);
  return true;
}


namespace rgw {
namespace auth {
namespace s3 {

std::string hash_string_sha256(const char* const data, const int len)
{
  std::string dest;
  calc_hash_sha256(data, len, dest);
  return dest;
}

/*
 * assemble canonical request for signature version 4
 */
static std::string assemble_v4_canonical_request(
  const char* const method,
  const char* const canonical_uri,
  const char* const canonical_qs,
  const char* const canonical_hdrs,
  const char* const signed_hdrs,
  const char* const request_payload_hash)
{
  std::string dest;

  if (method)
    dest = method;
  dest.append("\n");

  if (canonical_uri) {
    dest.append(canonical_uri);
  }
  dest.append("\n");

  if (canonical_qs) {
    dest.append(canonical_qs);
  }
  dest.append("\n");

  if (canonical_hdrs)
    dest.append(canonical_hdrs);
  dest.append("\n");

  if (signed_hdrs)
    dest.append(signed_hdrs);
  dest.append("\n");

  if (request_payload_hash)
    dest.append(request_payload_hash);

  return dest;
}

/*
 * create canonical request for signature version 4
 */
std::string get_v4_canonical_request_hash(CephContext* cct,
                                          const std::string& http_verb,
                                          const std::string& canonical_uri,
                                          const std::string& canonical_qs,
                                          const std::string& canonical_hdrs,
                                          const std::string& signed_hdrs,
                                          const std::string& request_payload_hash)
{
  ldout(cct, 10) << "payload request hash = " << request_payload_hash << dendl;

  std::string canonical_req = \
    assemble_v4_canonical_request(http_verb.c_str(),
                                  canonical_uri.c_str(),
                                  canonical_qs.c_str(),
                                  canonical_hdrs.c_str(),
                                  signed_hdrs.c_str(),
                                  request_payload_hash.c_str());

  std::string canonical_req_hash = \
    hash_string_sha256(canonical_req.c_str(), canonical_req.size());

  ldout(cct, 10) << "canonical request = " << canonical_req << dendl;
  ldout(cct, 10) << "canonical request hash = " << canonical_req_hash << dendl;

  return canonical_req_hash;
}

/*
 * assemble string to sign for signature version 4
 */
static std::string rgw_assemble_s3_v4_string_to_sign(
  const char* const algorithm,
  const char* const request_date,
  const char* const credential_scope,
  const char* const hashed_qr
) {
  std::string dest;

  if (algorithm)
    dest = algorithm;
  dest.append("\n");

  if (request_date)
    dest.append(request_date);
  dest.append("\n");

  if (credential_scope)
    dest.append(credential_scope);
  dest.append("\n");

  if (hashed_qr)
    dest.append(hashed_qr);

  return dest;
}

/*
 * create string to sign for signature version 4
 *
 * http://docs.aws.amazon.com/general/latest/gr/sigv4-create-string-to-sign.html
 */
std::string get_v4_string_to_sign(CephContext* const cct,
                                  const std::string& algorithm,
                                  const std::string& request_date,
                                  const std::string& credential_scope,
                                  const std::string& hashed_qr)
{
  const auto string_to_sign = \
    rgw_assemble_s3_v4_string_to_sign(
      algorithm.c_str(),
      request_date.c_str(),
      credential_scope.c_str(),
      hashed_qr.c_str());

  ldout(cct, 10) << "string to sign = "
                 << rgw::crypt_sanitize::log_content{string_to_sign.c_str()}
                 << dendl;
  return string_to_sign;
}

/*
 * calculate the AWS signature version 4
 */
std::string get_v4_signature(struct req_state* const s,
                             const std::string& access_key_id,
                             const std::string& date,
                             const std::string& region,
                             const std::string& service,
                             const std::string& string_to_sign,
                             const std::string& access_key_secret)
{
  std::string secret_key = "AWS4" + access_key_secret;
  char secret_k[secret_key.size() * MAX_UTF8_SZ];

  size_t n = 0;

  for (size_t i = 0; i < secret_key.size(); i++) {
    n += encode_utf8(secret_key[i], (unsigned char *) (secret_k + n));
  }

  string secret_key_utf8_k(secret_k, n);

  /* date */

  char date_k[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];
  calc_hmac_sha256(secret_key_utf8_k.c_str(), secret_key_utf8_k.size(),
      date.c_str(), date.size(), date_k);

  char aux[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE * 2 + 1];
  buf_to_hex((unsigned char *) date_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, aux);

  ldout(s->cct, 10) << "date_k        = " << string(aux) << dendl;

  /* region */

  char region_k[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];
  calc_hmac_sha256(date_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, region.c_str(), region.size(), region_k);

  buf_to_hex((unsigned char *) region_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, aux);

  ldout(s->cct, 10) << "region_k      = " << string(aux) << dendl;

  /* service */

  char service_k[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];
  calc_hmac_sha256(region_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, service.c_str(), service.size(), service_k);

  buf_to_hex((unsigned char *) service_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, aux);

  ldout(s->cct, 10) << "service_k     = " << string(aux) << dendl;

  /* aws4_request */

  char *signing_k = s->aws4_auth->signing_k;

  calc_hmac_sha256(service_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, "aws4_request", 12, signing_k);

  buf_to_hex((unsigned char *) signing_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, aux);

  ldout(s->cct, 10) << "signing_k     = " << string(aux) << dendl;

  /* TODO(rzarzynski): remove any modification to req_state! */
  s->aws4_auth->signing_key = aux;

  /* new signature */

  char signature_k[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];
  calc_hmac_sha256(signing_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, string_to_sign.c_str(), string_to_sign.size(), signature_k);

  buf_to_hex((unsigned char *) signature_k, CEPH_CRYPTO_HMACSHA256_DIGESTSIZE, aux);

  ldout(s->cct, 10) << "signature_k   = " << string(aux) << dendl;

  std::string signature = string(aux);

  ldout(s->cct, 10) << "new signature = " << signature << dendl;

  return signature;
}

} /* namespace s3 */
} /* namespace auth */
} /* namespace rgw */
