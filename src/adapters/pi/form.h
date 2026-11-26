#pragma once
#include <string>
#include <string_view>

namespace mico {

// omp's `ask` (18.x) takes one HTML <form>, {"html": "<form>…</form>"}, where
// earlier releases took a `questions` array. Read the way omp's own parser
// reads it (packages/tui/src/html-form/parse.ts): a radio group, a checkbox
// group or a <select> is a question, its options are the controls' labels,
// and its text is the field's label, else the text just before it, else its
// fieldset's legend, else its name.
//
// Writes the questions as a card reads them (parse_question_card): each with
// "question", "multi", "recommended" (the option checked at first) and
// "options" [{"label", "value"}], and a "form" object naming the control
// ("radio", "checkbox", "select", "multiselect"), the field's name and the
// options checked at first. A field mico cannot answer by choosing (a text
// box, a group whose options are all disabled, a control with no name) is
// written with "control": "text": the card is then shown, and answered in
// omp's terminal. False when the form has no named control.
bool omp_form_questions(std::string_view html, std::string& questions, std::string& first_question);

}  // namespace mico
