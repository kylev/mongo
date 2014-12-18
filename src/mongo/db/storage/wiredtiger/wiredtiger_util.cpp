// wiredtiger_util.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    using std::string;

    Status wtRCToStatus_slow(int retCode, const char* prefix ) {
        if (retCode == 0)
            return Status::OK();

        if ( retCode == WT_ROLLBACK ) {
            throw WriteConflictException();
        }

        fassert( 28559, retCode != WT_PANIC );

        str::stream s;
        if ( prefix )
            s << prefix << " ";
        s << retCode << ": " << wiredtiger_strerror(retCode);

        if (retCode == EINVAL) {
            return Status(ErrorCodes::BadValue, s);
        }

        // TODO convert specific codes rather than just using UNKNOWN_ERROR for everything.
        return Status(ErrorCodes::UnknownError, s);
    }

    StatusWith<std::string> WiredTigerUtil::getMetadata(OperationContext* opCtx,
                                                        const StringData& uri) {
        invariant(opCtx);
        WiredTigerCursor curwrap("metadata:", WiredTigerSession::kMetadataCursorId, opCtx);
        WT_CURSOR* cursor = curwrap.get();
        cursor->set_key(cursor, uri);
        int ret = cursor->search(cursor);
        if (ret == WT_NOTFOUND) {
            return StatusWith<std::string>(ErrorCodes::NoSuchKey, str::stream()
                << "Unable to find metadata for " << uri);
        }
        else if (ret != 0) {
            return StatusWith<std::string>(wtRCToStatus(ret));
        }
        const char* metadata = NULL;
        ret = cursor->get_value(cursor, &metadata);
        if (ret != 0) {
            return StatusWith<std::string>(wtRCToStatus(ret));
        }
        invariant(metadata);
        return StatusWith<std::string>(metadata);
    }

    Status WiredTigerUtil::getApplicationMetadata(OperationContext* opCtx,
                                                  const StringData& uri,
                                                  BSONObjBuilder* bob) {
        StatusWith<std::string> metadataResult = getMetadata(opCtx, uri);
        if (!metadataResult.isOK()) {
            return metadataResult.getStatus();
        }
        WiredTigerConfigParser topParser(metadataResult.getValue());
        WT_CONFIG_ITEM appMetadata;
        if (topParser.get("app_metadata", &appMetadata) != 0) {
            Status::OK();
        }
        if (appMetadata.len == 0) {
            return Status::OK();
        }
        if (appMetadata.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT) {
            return Status(ErrorCodes::FailedToParse, str::stream()
                << "app_metadata must be a nested struct. Actual value: "
                << StringData(appMetadata.str, appMetadata.len));
        }

        WiredTigerConfigParser parser(appMetadata);
        WT_CONFIG_ITEM keyItem;
        WT_CONFIG_ITEM valueItem;
        int ret;
        unordered_set<StringData, StringData::Hasher> keysSeen;
        while ((ret = parser.next(&keyItem, &valueItem)) == 0) {
            const StringData key(keyItem.str, keyItem.len);
            if (keysSeen.count(key)) {
                return Status(ErrorCodes::DuplicateKey, str::stream()
                    << "app_metadata must not contain duplicate keys. "
                    << "Found multiple instances of key '" << key << "'.");
            }
            keysSeen.insert(key);

            switch (valueItem.type) {
            case WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL:
                bob->appendBool(key, valueItem.val);
                break;
            case WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM:
                bob->appendIntOrLL(key, valueItem.val);
                break;
            default:
                bob->append(key, StringData(valueItem.str, valueItem.len));
                break;
            }
        }
        if (ret != WT_NOTFOUND) {
            return wtRCToStatus(ret);
        }

        return Status::OK();
    }

    StatusWith<BSONObj> WiredTigerUtil::getApplicationMetadata(OperationContext* opCtx,
                                                               const StringData& uri) {
        BSONObjBuilder bob;
        Status status = getApplicationMetadata(opCtx, uri, &bob);
        if (!status.isOK()) {
            return StatusWith<BSONObj>(status);
        }
        return StatusWith<BSONObj>(bob.obj());
    }

    Status WiredTigerUtil::checkApplicationMetadataFormatVersion(OperationContext* opCtx,
                                                                 const StringData& uri,
                                                                 int64_t minimumVersion,
                                                                 int64_t maximumVersion) {

        StatusWith<std::string> result = getMetadata(opCtx, uri);
        if (result.getStatus().code() == ErrorCodes::NoSuchKey) {
            return result.getStatus();
        }
        invariantOK(result.getStatus());

        WiredTigerConfigParser topParser(result.getValue());
        WT_CONFIG_ITEM metadata;
        if (topParser.get("app_metadata", &metadata) != 0)
            return Status(ErrorCodes::UnsupportedFormat, str::stream()
                          << "application metadata for " << uri
                          << " is missing ");

        WiredTigerConfigParser parser(metadata);

        int64_t version = 0;
        WT_CONFIG_ITEM versionItem;
        if (parser.get("formatVersion", &versionItem) != 0) {
            // If 'formatVersion' is missing, this metadata was introduced by
            // one of the RC versions (where the format version is 1).
            version = 1;
        }
        else if (versionItem.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM) {
            version = versionItem.val;
        }
        else {
            return Status(ErrorCodes::UnsupportedFormat, str::stream()
                << "'formatVersion' in application metadata for " << uri
                << " must be a number. Current value: "
                << StringData(versionItem.str, versionItem.len));
        }

        if (version < minimumVersion || version > maximumVersion) {
            return Status(ErrorCodes::UnsupportedFormat, str::stream()
                << "Application metadata for " << uri
                << " has unsupported format version " << version);
        }

        return Status::OK();
    }

    // static
    StatusWith<uint64_t> WiredTigerUtil::getStatisticsValue(WT_SESSION* session,
                                                            const std::string& uri,
                                                            const std::string& config,
                                                            int statisticsKey) {
        invariant(session);
        WT_CURSOR* cursor = NULL;
        const char* cursorConfig = config.empty() ? NULL : config.c_str();
        int ret = session->open_cursor(session, uri.c_str(), NULL, cursorConfig, &cursor);
        if (ret != 0) {
            return StatusWith<uint64_t>(ErrorCodes::CursorNotFound, str::stream()
                << "unable to open cursor at URI " << uri
                << ". reason: " << wiredtiger_strerror(ret));
        }
        invariant(cursor);
        ON_BLOCK_EXIT(cursor->close, cursor);

        cursor->set_key(cursor, statisticsKey);
        ret = cursor->search(cursor);
        if (ret != 0) {
            return StatusWith<uint64_t>(ErrorCodes::NoSuchKey, str::stream()
                << "unable to find key " << statisticsKey << " at URI " << uri
                << ". reason: " << wiredtiger_strerror(ret));
        }

        uint64_t value;
        ret = cursor->get_value(cursor, NULL, NULL, &value);
        if (ret != 0) {
            return StatusWith<uint64_t>(ErrorCodes::BadValue, str::stream()
                << "unable to get value for key " << statisticsKey << " at URI " << uri
                << ". reason: " << wiredtiger_strerror(ret));
        }

        return StatusWith<uint64_t>(value);
    }

    int64_t WiredTigerUtil::getIdentSize(WT_SESSION* s,
                                         const std::string& uri ) {
        StatusWith<int64_t> result = WiredTigerUtil::getStatisticsValueAs<int64_t>(
            s,
            "statistics:" + uri, "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
        const Status& status = result.getStatus();
        if ( !status.isOK() ) {
            if ( status.code() == ErrorCodes::CursorNotFound ) {
                // ident gone, so its 0
                return 0;
            }
            uassertStatusOK( status );
        }
        return result.getValue();
    }


    Status WiredTigerUtil::exportTableToBSON(WT_SESSION* session,
                                             const std::string& uri, const std::string& config,
                                             BSONObjBuilder* bob) {
        invariant(session);
        invariant(bob);
        WT_CURSOR* c = NULL;
        const char* cursorConfig = config.empty() ? NULL : config.c_str();
        int ret = session->open_cursor(session, uri.c_str(), NULL, cursorConfig, &c);
        if (ret != 0) {
            return Status(ErrorCodes::CursorNotFound, str::stream()
                << "unable to open cursor at URI " << uri
                << ". reason: " << wiredtiger_strerror(ret));
        }
        bob->append("uri", uri);
        invariant(c);
        ON_BLOCK_EXIT(c->close, c);

        std::map<string,BSONObjBuilder*> subs;
        const char* desc;
        uint64_t value;
        while (c->next(c) == 0 && c->get_value(c, &desc, NULL, &value) == 0) {
            StringData key( desc );

            StringData prefix;
            StringData suffix;

            size_t idx = key.find( ':' );
            if ( idx != string::npos ) {
                prefix = key.substr( 0, idx );
                suffix = key.substr( idx + 1 );
            }
            else {
                idx = key.find( ' ' );
            }

            if ( idx != string::npos ) {
                prefix = key.substr( 0, idx );
                suffix = key.substr( idx + 1 );
            }
            else {
                prefix = key;
                suffix = "num";
            }

            long long v = _castStatisticsValue<long long>(value);

            if ( prefix.size() == 0 ) {
                bob->appendNumber(desc, v);
            }
            else {
                BSONObjBuilder*& sub = subs[prefix.toString()];
                if ( !sub )
                    sub = new BSONObjBuilder();
                sub->appendNumber(mongoutils::str::ltrim(suffix.toString()), v);
            }
        }

        for ( std::map<string,BSONObjBuilder*>::const_iterator it = subs.begin();
              it != subs.end(); ++it ) {
            const std::string& s = it->first;
            bob->append( s, it->second->obj() );
            delete it->second;
        }
        return Status::OK();
    }

}  // namespace mongo
