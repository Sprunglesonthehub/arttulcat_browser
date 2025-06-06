/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const {
  workerTargetSpec,
} = require("resource://devtools/shared/specs/targets/worker.js");
const {
  FrontClassWithSpec,
  registerFront,
} = require("resource://devtools/shared/protocol.js");
const {
  TargetMixin,
} = require("resource://devtools/client/fronts/targets/target-mixin.js");

class WorkerTargetFront extends TargetMixin(
  FrontClassWithSpec(workerTargetSpec)
) {
  get isDedicatedWorker() {
    return this._type === Ci.nsIWorkerDebugger.TYPE_DEDICATED;
  }

  get isSharedWorker() {
    return this._type === Ci.nsIWorkerDebugger.TYPE_SHARED;
  }

  get isServiceWorker() {
    return this._type === Ci.nsIWorkerDebugger.TYPE_SERVICE;
  }

  // If the worker doesn't have a custom name,
  // display file name instead of absolute URL in the context selector/threads panel
  get name() {
    return this._name || this._url.split("/").pop();
  }

  form(json) {
    this.actorID = json.actor;

    // Save the full form for Target class usage.
    // Do not use `form` name to avoid colliding with protocol.js's `form` method
    this.targetForm = json;

    this._title = json.title;
    this._url = json.url;
    this._type = json.type;
    // Expose the WorkerDebugger's `id` so that we can match the target with the descriptor
    this.id = json.id;
    this._name = json.name;

    // Expose the inner Window ID of the document which may have spawned this worker
    this.relatedDocumentInnerWindowId = json.relatedDocumentInnerWindowId;
  }
}

exports.WorkerTargetFront = WorkerTargetFront;
registerFront(exports.WorkerTargetFront);
