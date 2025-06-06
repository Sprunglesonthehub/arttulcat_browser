/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

/**
 * Uses of this panel are:-
 * - Highlight line when source is opened using view source links from other panels
 * - Highlight line with function or class from an Outline search result selection
 * - Highlight line from a Quick open panel search result selection
 * - Highlight the last selected line when a source is selected
 * - Highlight the breakpoint line when the breakpoint is selected
 */

import { Component } from "devtools/client/shared/vendor/react";
import PropTypes from "devtools/client/shared/vendor/react-prop-types";
import { toEditorLine } from "../../utils/editor/index";

import { connect } from "devtools/client/shared/vendor/react-redux";
import {
  getVisibleSelectedFrame,
  getSelectedLocation,
  getSelectedSourceTextContent,
  getPauseCommand,
  getCurrentThread,
  getShouldHighlightSelectedLocation,
} from "../../selectors/index";
import { markerTypes } from "../../constants";

function isDebugLine(selectedFrame, selectedLocation) {
  if (!selectedFrame) {
    return false;
  }

  return (
    selectedFrame.location.source.id == selectedLocation.source.id &&
    selectedFrame.location.line == selectedLocation.line
  );
}

export class HighlightLine extends Component {
  isStepping = false;
  previousEditorLine = null;

  static get propTypes() {
    return {
      pauseCommand: PropTypes.oneOf([
        "expression",
        "resume",
        "stepOver",
        "stepIn",
        "stepOut",
      ]),
      selectedFrame: PropTypes.object,
      selectedLocation: PropTypes.object.isRequired,
      selectedSourceTextContent: PropTypes.object.isRequired,
      shouldHighlightSelectedLocation: PropTypes.bool.isRequired,
      editor: PropTypes.object,
    };
  }

  shouldComponentUpdate(nextProps) {
    return this.shouldSetHighlightLine(nextProps);
  }

  componentDidUpdate(prevProps) {
    this.highlightLine(prevProps);
  }

  componentDidMount() {
    this.highlightLine(null);
  }

  shouldSetHighlightLine({ selectedLocation, selectedSourceTextContent }) {
    const editorLine = toEditorLine(
      selectedLocation.source,
      selectedLocation.line
    );

    if (!selectedLocation || !selectedSourceTextContent) {
      return false;
    }

    if (this.isStepping && editorLine === this.previousEditorLine) {
      return false;
    }

    return true;
  }

  highlightLine(prevProps) {
    const { pauseCommand, shouldHighlightSelectedLocation } = this.props;
    if (pauseCommand) {
      this.isStepping = true;
    }

    if (prevProps) {
      this.clearHighlightLine(prevProps);
    }
    if (shouldHighlightSelectedLocation) {
      this.setHighlightLine();
    }
  }

  setHighlightLine() {
    const { selectedLocation, selectedFrame, editor } = this.props;
    if (!this.shouldSetHighlightLine(this.props)) {
      return;
    }

    this.isStepping = false;
    const editorLine = toEditorLine(
      selectedLocation.source,
      selectedLocation.line
    );
    this.previousEditorLine = editorLine;

    if (
      !selectedLocation.line ||
      isDebugLine(selectedFrame, selectedLocation)
    ) {
      return;
    }

    editor.setLineContentMarker({
      id: markerTypes.HIGHLIGHT_LINE_MARKER,
      lineClassName: "highlight-line",
      lines: [{ line: editorLine }],
    });
    this.clearHighlightLineAfterDuration();
  }

  clearHighlightLineAfterDuration() {
    const editorWrapper = document.querySelector(".editor-wrapper");

    if (editorWrapper === null) {
      return;
    }

    const duration = parseInt(
      getComputedStyle(editorWrapper).getPropertyValue(
        "--highlight-line-duration"
      ),
      10
    );

    setTimeout(() => this.clearHighlightLine(this.props), duration);
  }

  clearHighlightLine({ selectedLocation, selectedSourceTextContent }) {
    if (!selectedLocation || !selectedSourceTextContent) {
      return;
    }

    const { editor } = this.props;
    if (!editor) {
      return;
    }
    editor.removeLineContentMarker("highlight-line-marker");
  }

  render() {
    return null;
  }
}

export default connect(state => {
  const selectedLocation = getSelectedLocation(state);
  if (!selectedLocation) {
    return {};
  }
  return {
    pauseCommand: getPauseCommand(state, getCurrentThread(state)),
    shouldHighlightSelectedLocation: getShouldHighlightSelectedLocation(state),
    selectedFrame: getVisibleSelectedFrame(state),
    selectedLocation,
    selectedSourceTextContent: getSelectedSourceTextContent(state),
  };
})(HighlightLine);
