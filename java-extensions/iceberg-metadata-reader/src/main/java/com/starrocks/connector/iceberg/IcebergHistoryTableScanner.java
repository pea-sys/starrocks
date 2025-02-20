// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.connector.iceberg;

import com.google.common.collect.ImmutableSet;
import com.starrocks.jni.connector.ColumnValue;
import com.starrocks.utils.loader.ThreadContextClassLoader;
import org.apache.iceberg.Snapshot;
import org.apache.iceberg.util.SnapshotUtil;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

public class IcebergHistoryTableScanner extends AbstractIcebergMetadataScanner {
    private static final Logger LOG = LogManager.getLogger(IcebergHistoryTableScanner.class);

    private Iterator<Snapshot> reader;
    private Set<Long> ancestorIds;

    public IcebergHistoryTableScanner(int fetchSize, Map<String, String> params) {
        super(fetchSize, params);
    }

    @Override
    public void doOpen() {
        this.ancestorIds = ImmutableSet.copyOf(SnapshotUtil.currentAncestorIds(table));
    }

    @Override
    public void close() {
        if (reader != null) {
            reader = null;
        }
    }

    @Override
    public int getNext() throws IOException {
        try (ThreadContextClassLoader ignored = new ThreadContextClassLoader(classLoader)) {
            int numRows = 0;
            for (; numRows < getTableSize(); numRows++) {
                if (!reader.hasNext()) {
                    break;
                }
                Snapshot snapshot = reader.next();
                for (int i = 0; i < requiredFields.length; i++) {
                    Object fieldData = get(requiredFields[i], snapshot);
                    if (fieldData == null) {
                        appendData(i, null);
                    } else {
                        ColumnValue fieldValue = new IcebergMetadataColumnValue(fieldData, timezone);
                        appendData(i, fieldValue);
                    }
                }
            }
            return numRows;
        } catch (Exception e) {
            close();
            LOG.error("Failed to get the next off-heap table chunk of iceberg history table.", e);
            throw new IOException("Failed to get the next off-heap table chunk of iceberg history table.", e);
        }
    }

    @Override
    protected void initReader() {
        reader = table.snapshots().iterator();
    }

    private Object get(String columnName, Snapshot snapshot) {
        switch (columnName) {
            case "made_current_at":
                return snapshot.timestampMillis();
            case "snapshot_id":
                return snapshot.snapshotId();
            case "parent_id":
                return snapshot.parentId();
            case "is_current_ancestor":
                return ancestorIds.contains(snapshot.snapshotId()) ? "true" : "false";
            default:
                throw new IllegalArgumentException("Unrecognized column name " + columnName);
        }
    }
}
